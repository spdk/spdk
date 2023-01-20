/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <linux/ublk_cmd.h>
#include <liburing.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/json.h"
#include "spdk/ublk.h"
#include "spdk/thread.h"

#include "ublk_internal.h"

#define UBLK_CTRL_DEV			"/dev/ublk-control"
#define UBLK_BLK_CDEV			"/dev/ublkc"

#define LINUX_SECTOR_SHIFT		9
#define UBLK_CTRL_RING_DEPTH		32
#define UBLK_THREAD_MAX			128
#define UBLK_IO_MAX_BYTES		SPDK_BDEV_LARGE_BUF_MAX_SIZE
#define UBLK_DEV_MAX_QUEUES		32
#define UBLK_DEV_MAX_QUEUE_DEPTH	1024
#define UBLK_QUEUE_REQUEST		32
#define UBLK_STOP_BUSY_WAITING_MS	10000
#define UBLK_BUSY_POLLING_INTERVAL_US	20000

static uint32_t g_num_ublk_threads = 0;
static uint32_t g_queue_thread_id = 0;
static struct spdk_cpuset g_core_mask;

struct ublk_queue;
struct ublk_thread_ctx;
static void ublk_submit_bdev_io(struct ublk_queue *q, uint16_t tag);
static void ublk_dev_queue_fini(struct ublk_queue *q);
static int ublk_poll(void *arg);

struct ublk_io {
	void			*payload;
	uint32_t		payload_size;
	uint32_t		cmd_op;
	int32_t			result;
	bool			io_free;
	struct ublk_queue	*q;
	/* for bdev io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;

	TAILQ_ENTRY(ublk_io)	tailq;
};

struct ublk_queue {
	uint32_t		q_id;
	uint32_t		q_depth;
	struct ublk_io		*ios;
	TAILQ_HEAD(, ublk_io)	completed_io_list;
	TAILQ_HEAD(, ublk_io)	inflight_io_list;
	uint32_t		cmd_inflight;
	struct ublksrv_io_desc	*io_cmd_buf;
	/* ring depth == dev_info->queue_depth. */
	struct io_uring		ring;
	struct spdk_ublk_dev	*dev;
	struct ublk_thread_ctx	*thread_ctx;

	TAILQ_ENTRY(ublk_queue)	tailq;
};

struct spdk_ublk_dev {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch[UBLK_DEV_MAX_QUEUES];
	struct spdk_thread	*app_thread;

	int			cdev_fd;
	struct ublk_params	dev_params;
	struct ublksrv_ctrl_dev_info	dev_info;

	uint32_t		ublk_id;
	uint32_t		num_queues;
	uint32_t		queue_depth;

	struct spdk_mempool	*io_buf_pool;
	struct ublk_queue	queues[UBLK_DEV_MAX_QUEUES];

	/* Synchronize ublk_start_kernel pthread and ublk_stop */
	volatile int8_t		pending_ublk_pthread;
	struct spdk_poller	*retry_poller;
	int			retry_count;
	uint32_t		q_deinit_num;
	volatile bool		is_closing;
	ublk_del_cb		del_cb;
	void			*cb_arg;

	TAILQ_ENTRY(spdk_ublk_dev) tailq;
};

struct ublk_thread_ctx {
	struct spdk_thread		*ublk_thread;
	struct spdk_poller		*ublk_poller;
	TAILQ_HEAD(, ublk_queue)	queue_list;
};

struct ublk_tgt {
	int			ctrl_fd;
	bool			active;
	bool			is_destroying;
	spdk_ublk_fini_cb	cb_fn;
	void			*cb_arg;
	struct io_uring		ctrl_ring;
	struct ublk_thread_ctx	thread_ctx[UBLK_THREAD_MAX];
};

static TAILQ_HEAD(, spdk_ublk_dev) g_ublk_bdevs = TAILQ_HEAD_INITIALIZER(g_ublk_bdevs);
static struct ublk_tgt g_ublk_tgt;

/* helpers for using io_uring */
static inline int
ublk_setup_ring(uint32_t depth, struct io_uring *r, unsigned flags)
{
	struct io_uring_params p = {};

	p.flags = flags | IORING_SETUP_CQSIZE;
	p.cq_entries = depth;

	return io_uring_queue_init_params(depth, r, &p);
}

static inline struct io_uring_sqe *
ublk_uring_get_sqe(struct io_uring *r, uint32_t idx)
{
	/* Need to update the idx since we set IORING_SETUP_SQE128 parameter in ublk_setup_ring */
	return &r->sq.sqes[idx << 1];
}

static inline void *
ublk_get_sqe_cmd(struct io_uring_sqe *sqe)
{
	return (void *)&sqe->addr3;
}

static inline void
ublk_set_sqe_cmd_op(struct io_uring_sqe *sqe, uint32_t cmd_op)
{
	sqe->off = cmd_op;
}

static inline uint64_t
build_user_data(uint16_t tag, uint8_t op)
{
	assert(!(tag >> 16) && !(op >> 8));

	return tag | (op << 16);
}

static inline uint16_t
user_data_to_tag(uint64_t user_data)
{
	return user_data & 0xffff;
}

static inline uint8_t
user_data_to_op(uint64_t user_data)
{
	return (user_data >> 16) & 0xff;
}

void
spdk_ublk_init(void)
{
	uint32_t i;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_cpuset_zero(&g_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_core_mask, i, true);
	}
}

static int
ublk_ctrl_cmd(struct spdk_ublk_dev *ublk, uint32_t cmd_op, void *args)
{
	uint32_t dev_id = ublk->ublk_id;
	int rc = -EINVAL;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct ublksrv_ctrl_cmd *cmd;
	int *ublk_pid;
	struct ublksrv_ctrl_dev_info *dev_info;
	struct ublk_params *params;

	sqe = io_uring_get_sqe(&g_ublk_tgt.ctrl_ring);
	if (!sqe) {
		SPDK_ERRLOG("can't get sqe rc %d\n", rc);
		return rc;
	}
	cmd = (struct ublksrv_ctrl_cmd *)ublk_get_sqe_cmd(sqe);
	sqe->fd = g_ublk_tgt.ctrl_fd;
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->ioprio = 0;
	cmd->dev_id = dev_id;
	cmd->queue_id = -1;

	switch (cmd_op) {
	case UBLK_CMD_START_DEV:
		ublk_pid = args;
		cmd->data[0] = *ublk_pid;
		cmd->data[1] = 0;
		break;
	case UBLK_CMD_ADD_DEV:
		dev_info = args;
		cmd->addr = (__u64)(uintptr_t)(dev_info);
		cmd->len = sizeof(*dev_info);
		break;
	case UBLK_CMD_STOP_DEV:
	case UBLK_CMD_DEL_DEV:
		break;
	case UBLK_CMD_SET_PARAMS:
		params = args;
		cmd->addr = (__u64)(uintptr_t)params;
		cmd->len = sizeof(*params);
		break;
	default:
		SPDK_ERRLOG("No match cmd operation,cmd_op = %d\n", cmd_op);
		return -EINVAL;
	}
	ublk_set_sqe_cmd_op(sqe, cmd_op);
	io_uring_sqe_set_data(sqe, cmd);

	rc = io_uring_submit(&g_ublk_tgt.ctrl_ring);
	if (rc < 0) {
		SPDK_ERRLOG("uring submit rc %d\n", rc);
		return rc;
	}

	rc = io_uring_wait_cqe(&g_ublk_tgt.ctrl_ring, &cqe);
	if (rc < 0) {
		SPDK_ERRLOG("wait cqe: %s\n", strerror(-rc));
		return rc;
	}
	io_uring_cqe_seen(&g_ublk_tgt.ctrl_ring, cqe);

	return cqe->res;
}

static int
ublk_queue_cmd_buf_sz(uint32_t q_depth)
{
	uint32_t size = q_depth * sizeof(struct ublksrv_io_desc);
	uint32_t page_sz = getpagesize();

	/* round up size */
	return (size + page_sz - 1) & ~(page_sz - 1);
}

static int
ublk_open(void)
{
	int rc;

	g_ublk_tgt.ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (g_ublk_tgt.ctrl_fd < 0) {
		rc = errno;
		SPDK_ERRLOG("UBLK conrol dev %s can't be opened, error=%s\n", UBLK_CTRL_DEV, spdk_strerror(errno));
		return -rc;
	}

	rc = ublk_setup_ring(UBLK_CTRL_RING_DEPTH, &g_ublk_tgt.ctrl_ring, IORING_SETUP_SQE128);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK ctrl queue_init: %s\n", spdk_strerror(-rc));
		close(g_ublk_tgt.ctrl_fd);
		return rc;
	}

	return 0;
}

static int
ublk_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;
	struct spdk_cpuset tmp_mask;

	if (cpumask == NULL) {
		return -EPERM;
	}

	if (mask == NULL) {
		spdk_cpuset_copy(cpumask, &g_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(cpumask, mask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -rc;
	}

	if (spdk_cpuset_count(cpumask) == 0) {
		SPDK_ERRLOG("no cpus specified\n");
		return -EINVAL;
	}

	spdk_cpuset_copy(&tmp_mask, cpumask);
	spdk_cpuset_and(&tmp_mask, &g_core_mask);

	if (!spdk_cpuset_equal(&tmp_mask, cpumask)) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_core_mask));
		return -EINVAL;
	}

	return 0;
}

static void
ublk_poller_register(void *args)
{
	struct ublk_thread_ctx *thread_ctx = args;

	assert(spdk_get_thread() == thread_ctx->ublk_thread);
	TAILQ_INIT(&thread_ctx->queue_list);
	thread_ctx->ublk_poller = SPDK_POLLER_REGISTER(ublk_poll, thread_ctx, 0);
}

int
ublk_create_target(const char *cpumask_str)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct spdk_cpuset cpuset = {};
	struct spdk_cpuset thd_cpuset = {};
	struct ublk_thread_ctx *thread_ctx;

	if (g_ublk_tgt.active == true) {
		SPDK_ERRLOG("UBLK target has been created\n");
		return -EBUSY;
	}

	rc = ublk_parse_core_mask(cpumask_str, &cpuset);
	if (rc != 0) {
		return rc;
	}

	rc = ublk_open();
	if (rc != 0) {
		SPDK_ERRLOG("Fail to open UBLK, error=%s\n", spdk_strerror(-rc));
		return rc;
	}

	SPDK_ENV_FOREACH_CORE(i) {
		if (spdk_cpuset_get_cpu(&cpuset, i)) {
			spdk_cpuset_zero(&thd_cpuset);
			spdk_cpuset_set_cpu(&thd_cpuset, i, true);
			snprintf(thread_name, sizeof(thread_name), "ublk_thread%u", i);
			thread_ctx = &g_ublk_tgt.thread_ctx[g_num_ublk_threads];
			thread_ctx->ublk_thread = spdk_thread_create(thread_name, &thd_cpuset);
			spdk_thread_send_msg(thread_ctx->ublk_thread, ublk_poller_register, thread_ctx);
			g_num_ublk_threads++;
		}
	}
	g_ublk_tgt.active = true;
	SPDK_NOTICELOG("UBLK target created successfully\n");

	return 0;
}

static void
_ublk_fini_done(void *args)
{
	g_num_ublk_threads = 0;
	g_queue_thread_id = 0;
	g_ublk_tgt.is_destroying = false;
	g_ublk_tgt.active = false;
	if (g_ublk_tgt.cb_fn) {
		g_ublk_tgt.cb_fn(g_ublk_tgt.cb_arg);
		g_ublk_tgt.cb_fn = NULL;
		g_ublk_tgt.cb_arg = NULL;
	}
}

static void
ublk_thread_exit(void *args)
{
	struct spdk_thread *ublk_thread = spdk_get_thread();
	uint32_t i;

	for (i = 0; i < g_num_ublk_threads; i++) {
		if (g_ublk_tgt.thread_ctx[i].ublk_thread == ublk_thread) {
			spdk_poller_unregister(&g_ublk_tgt.thread_ctx[i].ublk_poller);
			spdk_thread_exit(ublk_thread);
		}
	}
}

static void *
_ublk_start_kernel(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;
	int rc;

	SPDK_DEBUGLOG(ublk, "Enter start pthread for ublk %d\n", ublk->ublk_id);
	assert(ublk->dev_info.ublksrv_pid == getpid());
	rc = ublk_ctrl_cmd(ublk, UBLK_CMD_START_DEV, &ublk->dev_info.ublksrv_pid);
	if (rc < 0) {
		SPDK_ERRLOG("start dev %d failed, rc %s\n", ublk->ublk_id,
			    spdk_strerror(-rc));
	}
	__atomic_fetch_sub(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
	SPDK_DEBUGLOG(ublk, "Exit start pthread for ublk %d\n", ublk->ublk_id);

	pthread_exit(NULL);
}

static int
ublk_start_kernel(struct spdk_ublk_dev *ublk)
{
	pthread_t tid;
	int rc;

	__atomic_fetch_add(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
	/*
	 * Commands UBLK_CMD_START_DEV will block the reactor while some io are
	 * transfered to SPDK for processing, which leads hanging. To avoid that,
	 * a pthread needs to be created. It's also the same scenario for UBLK_CMD_STOP_DEV.
	 */
	rc = pthread_create(&tid, NULL, _ublk_start_kernel, ublk);
	if (rc != 0) {
		__atomic_fetch_sub(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		return -rc;
	}
	rc = pthread_detach(tid);
	assert(rc == 0);

	return 0;
}

static void *
_ublk_stop_kernel(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;
	int rc;

	SPDK_DEBUGLOG(ublk, "Enter stop pthread for ublk %d\n", ublk->ublk_id);

	rc = ublk_ctrl_cmd(ublk, UBLK_CMD_STOP_DEV, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("stop dev %d failed\n", ublk->ublk_id);
	}
	__atomic_fetch_sub(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
	SPDK_DEBUGLOG(ublk, "Exit stop pthread for ublk %d\n", ublk->ublk_id);

	pthread_exit(NULL);
}

static int
ublk_stop_kernel(struct spdk_ublk_dev *ublk)
{
	pthread_t tid;
	int rc;

	__atomic_fetch_add(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
	rc = pthread_create(&tid, NULL, _ublk_stop_kernel, ublk);
	if (rc != 0) {
		__atomic_fetch_sub(&ublk->pending_ublk_pthread, 1, __ATOMIC_RELAXED);
		SPDK_ERRLOG("could not create thread: %s\n", spdk_strerror(rc));
		return -rc;
	}
	rc = pthread_detach(tid);
	assert(rc == 0);

	return 0;
}

static int
ublk_close_dev(struct spdk_ublk_dev *ublk)
{
	/* set is_closing */
	if (ublk->is_closing) {
		return -EBUSY;
	}
	ublk->is_closing = true;

	return ublk_stop_kernel(ublk);
}

static void
_ublk_fini(void *args)
{
	struct spdk_ublk_dev	*ublk, *ublk_tmp;

	TAILQ_FOREACH_SAFE(ublk, &g_ublk_bdevs, tailq, ublk_tmp) {
		ublk_close_dev(ublk);
	}

	/* Check if all ublks closed */
	if (TAILQ_EMPTY(&g_ublk_bdevs)) {
		close(g_ublk_tgt.ctrl_ring.ring_fd);
		close(g_ublk_tgt.ctrl_fd);
		g_ublk_tgt.ctrl_ring.ring_fd = 0;
		g_ublk_tgt.ctrl_fd = 0;
		spdk_for_each_thread(ublk_thread_exit, NULL, _ublk_fini_done);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), _ublk_fini, NULL);
	}
}

int
spdk_ublk_fini(spdk_ublk_fini_cb cb_fn, void *cb_arg)
{
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (g_ublk_tgt.is_destroying == true) {
		/* UBLK target is being destroying */
		return -EBUSY;
	}
	g_ublk_tgt.cb_fn = cb_fn;
	g_ublk_tgt.cb_arg = cb_arg;
	g_ublk_tgt.is_destroying = true;
	_ublk_fini(NULL);

	return 0;
}

int
ublk_destroy_target(spdk_ublk_fini_cb cb_fn, void *cb_arg)
{
	int rc;

	if (g_ublk_tgt.active == false) {
		/* UBLK target has not been created */
		return -ENOENT;
	}

	rc = spdk_ublk_fini(cb_fn, cb_arg);

	return rc;
}

struct spdk_ublk_dev *
ublk_dev_find_by_id(uint32_t ublk_id)
{
	struct spdk_ublk_dev *ublk;

	/* check whether ublk has already been registered by ublk path. */
	TAILQ_FOREACH(ublk, &g_ublk_bdevs, tailq) {
		if (ublk->ublk_id == ublk_id) {
			return ublk;
		}
	}

	return NULL;
}

uint32_t
ublk_dev_get_id(struct spdk_ublk_dev *ublk)
{
	return ublk->ublk_id;
}

struct spdk_ublk_dev *ublk_dev_first(void)
{
	return TAILQ_FIRST(&g_ublk_bdevs);
}

struct spdk_ublk_dev *ublk_dev_next(struct spdk_ublk_dev *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

uint32_t
ublk_dev_get_queue_depth(struct spdk_ublk_dev *ublk)
{
	return ublk->queue_depth;
}

uint32_t
ublk_dev_get_num_queues(struct spdk_ublk_dev *ublk)
{
	return ublk->num_queues;
}

const char *
ublk_dev_get_bdev_name(struct spdk_ublk_dev *ublk)
{
	return spdk_bdev_get_name(ublk->bdev);
}

void
spdk_ublk_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_ublk_dev *ublk;

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(ublk, &g_ublk_bdevs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "ublk_start_disk");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "bdev_name", ublk_dev_get_bdev_name(ublk));
		spdk_json_write_named_uint32(w, "ublk_id", ublk->ublk_id);
		spdk_json_write_named_uint32(w, "num_queues", ublk->num_queues);
		spdk_json_write_named_uint32(w, "queue_depth", ublk->queue_depth);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static int
ublk_dev_list_register(struct spdk_ublk_dev *ublk)
{
	/* Make sure ublk_id is not used in this SPDK app */
	if (ublk_dev_find_by_id(ublk->ublk_id)) {
		SPDK_NOTICELOG("%d is already exported with bdev %s\n",
			       ublk->ublk_id, ublk_dev_get_bdev_name(ublk));
		return -EBUSY;
	}

	TAILQ_INSERT_TAIL(&g_ublk_bdevs, ublk, tailq);

	return 0;
}

static void
ublk_dev_list_unregister(struct spdk_ublk_dev *ublk)
{
	/*
	 * ublk device may be stopped before registered.
	 * check whether it was registered.
	 */

	if (ublk_dev_find_by_id(ublk->ublk_id)) {
		TAILQ_REMOVE(&g_ublk_bdevs, ublk, tailq);
	}
}

static inline bool
ublk_is_ready_to_stop(struct spdk_ublk_dev *ublk)
{
	/*
	 * Stop action should be called only after all ublk_io are completed.
	 */
	bool ready_to_stop = true;
	struct ublk_queue *q;
	uint32_t i;

	for (i = 0; i < ublk->num_queues; i++) {
		q = &ublk->queues[i];
		if (!TAILQ_EMPTY(&q->inflight_io_list) || !TAILQ_EMPTY(&q->completed_io_list) || q->cmd_inflight) {
			ready_to_stop = false;
			break;
		}
	}

	return ready_to_stop;
}

static void
ublk_close_dev_done(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;
	struct ublk_queue *q;
	int rc = 0;
	uint32_t i, q_idx;

	assert(spdk_get_thread() == ublk->app_thread);
	for (q_idx = 0; q_idx < ublk->num_queues; q_idx++) {
		q = &ublk->queues[q_idx];
		ublk_dev_queue_fini(q);
		for (i = 0; i < q->q_depth; i++) {
			if (q->ios[i].payload) {
				spdk_mempool_put(ublk->io_buf_pool, q->ios[i].payload);
				q->ios[i].payload = NULL;
			}
		}
		free(q->ios);
	}

	spdk_mempool_free(ublk->io_buf_pool);
	if (ublk->cdev_fd >= 0) {
		close(ublk->cdev_fd);
	}

	rc = ublk_ctrl_cmd(ublk, UBLK_CMD_DEL_DEV, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("delete dev %d failed\n", ublk->ublk_id);
	}

	if (ublk->bdev_desc) {
		spdk_bdev_close(ublk->bdev_desc);
		ublk->bdev_desc = NULL;
	}

	ublk_dev_list_unregister(ublk);

	if (ublk->del_cb) {
		ublk->del_cb(ublk->cb_arg);
	}
	SPDK_NOTICELOG("ublk dev %d stopped\n", ublk->ublk_id);
	free(ublk);
}

static int
_ublk_try_close_dev(void *arg)
{
	struct spdk_ublk_dev *ublk = arg;

	assert(spdk_get_thread() == ublk->app_thread);
	/* Continue the stop procedure after the exit of ublk_start_kernel pthread */
	if (__atomic_load_n(&ublk->pending_ublk_pthread, __ATOMIC_RELAXED) > 0) {
		if (ublk->retry_poller == NULL) {
			ublk->retry_count = UBLK_STOP_BUSY_WAITING_MS * 1000ULL / UBLK_BUSY_POLLING_INTERVAL_US;
			ublk->retry_poller = SPDK_POLLER_REGISTER(_ublk_try_close_dev, ublk,
					     UBLK_BUSY_POLLING_INTERVAL_US);
			return SPDK_POLLER_BUSY;
		}
		if (ublk->retry_count-- > 0) {
			return SPDK_POLLER_BUSY;
		}
		SPDK_ERRLOG("Failed to wait for returning of ublk pthread.\n");
	}
	spdk_poller_unregister(&ublk->retry_poller);
	/* Update queue deinit number */
	ublk->q_deinit_num += 1;
	if (ublk->q_deinit_num == ublk->num_queues) {
		spdk_thread_send_msg(ublk->app_thread, ublk_close_dev_done, ublk);
	}

	return SPDK_POLLER_BUSY;
}

static void
ublk_try_close_dev(void *arg)
{
	_ublk_try_close_dev(arg);
}

static void
ublk_try_close_queue(void *arg)
{
	struct ublk_queue *q = arg;
	struct spdk_ublk_dev *ublk = q->dev;

	if (!ublk_is_ready_to_stop(ublk)) {
		/* wait for next retry */
		return;
	}
	TAILQ_REMOVE(&q->thread_ctx->queue_list, q, tailq);
	spdk_put_io_channel(ublk->ch[q->q_id]);
	ublk->ch[q->q_id] = NULL;

	spdk_thread_send_msg(ublk->app_thread, ublk_try_close_dev, ublk);
}

int
ublk_stop_disk(uint32_t ublk_id, ublk_del_cb del_cb, void *cb_arg)
{
	struct spdk_ublk_dev *ublk;

	ublk = ublk_dev_find_by_id(ublk_id);
	if (ublk == NULL) {
		SPDK_ERRLOG("no ublk dev with ublk_id=%u\n", ublk_id);
		return -ENODEV;
	}
	if (ublk->is_closing) {
		SPDK_WARNLOG("ublk %d is closing\n", ublk->ublk_id);
		return -EBUSY;
	}

	ublk->del_cb = del_cb;
	ublk->cb_arg = cb_arg;
	return ublk_close_dev(ublk);
}

static inline void
ublk_mark_io_get_data(struct ublk_io *io)
{
	io->cmd_op = UBLK_IO_NEED_GET_DATA;
	io->result = 0;
}

static inline void
ublk_mark_io_done(struct ublk_io *io, int res)
{
	/*
	 * mark io done by target, so that SPDK can commit its
	 * result and fetch new request via io_uring command.
	 */
	io->cmd_op = UBLK_IO_COMMIT_AND_FETCH_REQ;
	io->io_free = true;
	io->result = res;
}

static void
ublk_io_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ublk_io	*io = cb_arg;
	struct ublk_queue *q = io->q;
	int res;

	if (success) {
		res = io->result;
	} else {
		res = -EIO;
	}

	ublk_mark_io_done(io, res);

	SPDK_DEBUGLOG(ublk, "(qid %d tag %d res %d)\n",
		      q->q_id, (int)(io - q->ios), res);
	TAILQ_REMOVE(&q->inflight_io_list, io, tailq);
	TAILQ_INSERT_TAIL(&q->completed_io_list, io, tailq);

	if (bdev_io != NULL) {
		spdk_bdev_free_io(bdev_io);
	}
}

static void
ublk_resubmit_io(void *arg)
{
	struct ublk_io *io = (struct ublk_io *)arg;
	uint16_t tag = (io - io->q->ios);

	ublk_submit_bdev_io(io->q, tag);
}

static void
ublk_queue_io(struct ublk_io *io)
{
	int rc;
	struct spdk_bdev *bdev = io->q->dev->bdev;
	struct ublk_queue *q = io->q;

	io->bdev_io_wait.bdev = bdev;
	io->bdev_io_wait.cb_fn = ublk_resubmit_io;
	io->bdev_io_wait.cb_arg = io;

	rc = spdk_bdev_queue_io_wait(bdev, q->dev->ch[q->q_id], &io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in ublk_queue_io, rc=%d.\n", rc);
		ublk_io_done(NULL, false, io);
	}
}

static void
ublk_submit_bdev_io(struct ublk_queue *q, uint16_t tag)
{
	struct spdk_ublk_dev *ublk = q->dev;
	struct ublk_io *io = &q->ios[tag];
	struct spdk_bdev_desc *desc = ublk->bdev_desc;
	struct spdk_io_channel *ch = ublk->ch[q->q_id];
	uint64_t offset_blocks, num_blocks;
	uint8_t ublk_op;
	uint32_t sector_per_block, sector_per_block_shift;
	void *payload;
	int rc = 0;
	const struct ublksrv_io_desc *iod = &q->io_cmd_buf[tag];

	ublk_op = ublksrv_get_op(iod);
	sector_per_block = spdk_bdev_get_data_block_size(ublk->bdev) >> LINUX_SECTOR_SHIFT;
	sector_per_block_shift = spdk_u32log2(sector_per_block);
	offset_blocks = iod->start_sector >> sector_per_block_shift;
	num_blocks = iod->nr_sectors >> sector_per_block_shift;
	payload = (void *)iod->addr;

	io->result = num_blocks * spdk_bdev_get_data_block_size(ublk->bdev);
	switch (ublk_op) {
	case UBLK_IO_OP_READ:
		rc = spdk_bdev_read_blocks(desc, ch, payload, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_WRITE:
		rc = spdk_bdev_write_blocks(desc, ch, payload, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_FLUSH:
		rc = spdk_bdev_flush_blocks(desc, ch, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_DISCARD:
		rc = spdk_bdev_unmap_blocks(desc, ch, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	case UBLK_IO_OP_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(desc, ch, offset_blocks, num_blocks, ublk_io_done, io);
		break;
	default:
		rc = -1;
	}

	if (rc < 0) {
		if (rc == -ENOMEM) {
			SPDK_INFOLOG(ublk, "No memory, start to queue io.\n");
			ublk_queue_io(io);
		} else {
			SPDK_ERRLOG("ublk io failed in ublk_queue_io, rc=%d.\n", rc);
			ublk_io_done(NULL, false, io);
		}
	}
}

static inline void
ublksrv_queue_io_cmd(struct ublk_queue *q,
		     struct ublk_io *io, unsigned tag)
{
	struct ublksrv_io_cmd *cmd;
	struct io_uring_sqe *sqe;
	unsigned int cmd_op = 0;;
	uint64_t user_data;

	/* check io status is free and each io should have operation of fetching or committing */
	assert(io->io_free);
	assert((io->cmd_op == UBLK_IO_FETCH_REQ) || (io->cmd_op == UBLK_IO_NEED_GET_DATA) ||
	       (io->cmd_op == UBLK_IO_COMMIT_AND_FETCH_REQ));
	cmd_op = io->cmd_op;

	sqe = io_uring_get_sqe(&q->ring);
	assert(sqe);

	cmd = (struct ublksrv_io_cmd *)ublk_get_sqe_cmd(sqe);
	if (cmd_op == UBLK_IO_COMMIT_AND_FETCH_REQ) {
		cmd->result = io->result;
	}

	/* These fields should be written once, never change */
	ublk_set_sqe_cmd_op(sqe, cmd_op);
	/* dev->cdev_fd */
	sqe->fd		= 0;
	sqe->opcode	= IORING_OP_URING_CMD;
	sqe->flags	= IOSQE_FIXED_FILE;
	sqe->rw_flags	= 0;
	cmd->tag	= tag;
	cmd->addr	= (__u64)(uintptr_t)(io->payload);
	cmd->q_id	= q->q_id;

	user_data = build_user_data(tag, cmd_op);
	io_uring_sqe_set_data64(sqe, user_data);

	io->cmd_op = 0;
	q->cmd_inflight += 1;

	SPDK_DEBUGLOG(ublk, "(qid %d tag %u cmd_op %u) iof %x stopping %d\n",
		      q->q_id, tag, cmd_op,
		      io->cmd_op, q->dev->is_closing);
}

static int
ublk_io_xmit(struct ublk_queue *q)
{
	int rc = 0, count = 0, tag;
	struct ublk_io *io;

	if (TAILQ_EMPTY(&q->completed_io_list)) {
		return 0;
	}

	while (!TAILQ_EMPTY(&q->completed_io_list)) {
		io = TAILQ_FIRST(&q->completed_io_list);
		tag = io - io->q->ios;
		assert(io != NULL);
		/*
		 * Remove IO from list now assuming it will be completed. It will be inserted
		 * back to the head if it cannot be completed. This approach is specifically
		 * taken to work around a scan-build use-after-free mischaracterization.
		 */
		TAILQ_REMOVE(&q->completed_io_list, io, tailq);
		ublksrv_queue_io_cmd(q, io, tag);
		count++;
	}

	rc = io_uring_submit(&q->ring);
	assert(rc == count);

	return rc;
}

static int
ublk_io_recv(struct ublk_queue *q)
{
	struct io_uring_cqe *cqe;
	unsigned head, tag;
	int fetch, count = 0;
	struct ublk_io *io;
	struct spdk_ublk_dev *dev = q->dev;
	unsigned __attribute__((unused)) cmd_op;

	if (q->cmd_inflight == 0) {
		return 0;
	}

	io_uring_for_each_cqe(&q->ring, head, cqe) {
		tag = user_data_to_tag(cqe->user_data);
		cmd_op = user_data_to_op(cqe->user_data);
		fetch = (cqe->res != UBLK_IO_RES_ABORT) && !dev->is_closing;

		SPDK_DEBUGLOG(ublk, "res %d qid %d tag %u cmd_op %u\n",
			      cqe->res, q->q_id, tag, cmd_op);

		q->cmd_inflight--;
		io = &q->ios[tag];

		if (!fetch) {
			dev->is_closing = true;
			if (io->cmd_op == UBLK_IO_FETCH_REQ) {
				io->cmd_op = 0;
			}
		}

		TAILQ_INSERT_TAIL(&q->inflight_io_list, io, tailq);
		if (cqe->res == UBLK_IO_RES_OK) {
			ublk_submit_bdev_io(q, tag);
		} else if (cqe->res == UBLK_IO_RES_NEED_GET_DATA) {
			ublk_mark_io_get_data(io);
			TAILQ_REMOVE(&q->inflight_io_list, io, tailq);
			TAILQ_INSERT_TAIL(&q->completed_io_list, io, tailq);

		} else {
			if (cqe->res != UBLK_IO_RES_ABORT) {
				SPDK_ERRLOG("ublk received error io: res %d qid %d tag %u cmd_op %u\n",
					    cqe->res, q->q_id, tag, cmd_op);
			}
			io->io_free = true;
			TAILQ_REMOVE(&q->inflight_io_list, io, tailq);
		}
		count += 1;
		if (count == UBLK_QUEUE_REQUEST) {
			break;
		}
	}
	io_uring_cq_advance(&q->ring, count);

	return count;
}

static int
ublk_poll(void *arg)
{
	struct ublk_thread_ctx *thread_ctx = arg;
	struct ublk_queue *q, *q_tmp;
	struct spdk_ublk_dev *ublk;
	int sent, received, count = 0;

	TAILQ_FOREACH_SAFE(q, &thread_ctx->queue_list, tailq, q_tmp) {
		sent = ublk_io_xmit(q);
		received = ublk_io_recv(q);
		ublk = q->dev;
		if (spdk_unlikely(ublk->is_closing)) {
			ublk_try_close_queue(q);
		}
		count += sent + received;
	}
	if (count > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static void
ublk_bdev_hot_remove(struct spdk_ublk_dev *ublk)
{
	ublk_close_dev(ublk);
}

static void
ublk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		ublk_bdev_hot_remove(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
ublk_dev_init_io_cmds(struct io_uring *r, uint32_t q_depth)
{
	struct io_uring_sqe *sqe;
	uint32_t i;

	for (i = 0; i < q_depth; i++) {
		sqe = ublk_uring_get_sqe(r, i);

		/* These fields should be written once, never change */
		sqe->flags = IOSQE_FIXED_FILE;
		sqe->rw_flags = 0;
		sqe->ioprio = 0;
		sqe->off = 0;
	}
}

static int
ublk_dev_queue_init(struct ublk_queue *q)
{
	int rc = 0, cmd_buf_size;
	uint32_t j;
	struct spdk_ublk_dev *ublk = q->dev;
	unsigned long off;

	cmd_buf_size = ublk_queue_cmd_buf_sz(q->q_depth);
	off = UBLKSRV_CMD_BUF_OFFSET +
	      q->q_id * (UBLK_MAX_QUEUE_DEPTH * sizeof(struct ublksrv_io_desc));
	q->io_cmd_buf = (struct ublksrv_io_desc *)mmap(0, cmd_buf_size, PROT_READ,
			MAP_SHARED | MAP_POPULATE, ublk->cdev_fd, off);
	if (q->io_cmd_buf == MAP_FAILED) {
		q->io_cmd_buf = NULL;
		rc = -errno;
		SPDK_ERRLOG("Failed at mmap: %s\n", spdk_strerror(-rc));
		goto err;
	}

	for (j = 0; j < q->q_depth; j++) {
		q->ios[j].cmd_op = UBLK_IO_FETCH_REQ;
		q->ios[j].io_free = true;
	}

	rc = ublk_setup_ring(q->q_depth, &q->ring, IORING_SETUP_SQE128);
	if (rc < 0) {
		SPDK_ERRLOG("Failed at setup uring: %s\n", spdk_strerror(-rc));
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
		goto err;
	}

	rc = io_uring_register_files(&q->ring, &ublk->cdev_fd, 1);
	if (rc != 0) {
		SPDK_ERRLOG("Failed at uring register files: %s\n", spdk_strerror(-rc));
		close(q->ring.ring_fd);
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
		goto err;
	}

	ublk_dev_init_io_cmds(&q->ring, q->q_depth);

	return 0;
err:
	return rc;
}

static void
ublk_dev_queue_fini(struct ublk_queue *q)
{
	io_uring_unregister_files(&q->ring);
	close(q->ring.ring_fd);
	munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q->q_depth));
}

static void
ublk_dev_queue_io_init(struct ublk_queue *q)
{
	uint32_t i;
	int rc __attribute__((unused));

	/* submit all io commands to ublk driver */
	for (i = 0; i < q->q_depth; i++) {
		ublksrv_queue_io_cmd(q, &q->ios[i], i);
	}

	rc = io_uring_submit(&q->ring);
	assert(rc == (int)q->q_depth);
}

static int
_ublk_start_disk(struct spdk_ublk_dev *ublk)
{
	int rc;

	rc = ublk_ctrl_cmd(ublk, UBLK_CMD_ADD_DEV, &ublk->dev_info);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK can't add dev %d, rc %s\n", ublk->ublk_id, spdk_strerror(-rc));
		goto err;
	}

	ublk->dev_params.len = sizeof(struct ublk_params);
	rc = ublk_ctrl_cmd(ublk, UBLK_CMD_SET_PARAMS, &ublk->dev_params);
	if (rc < 0) {
		SPDK_ERRLOG("UBLK can't set params for dev %d, rc %s\n", ublk->ublk_id, spdk_strerror(-rc));
		goto err;
	}

	return 0;

err:
	return rc;
}

/* Set ublk device parameters based on bdev */
static void
ublk_info_param_init(struct spdk_ublk_dev *ublk)
{
	struct spdk_bdev *bdev = ublk->bdev;
	uint32_t blk_size = spdk_bdev_get_data_block_size(bdev);
	uint32_t pblk_size = spdk_bdev_get_physical_block_size(bdev);
	uint32_t io_opt_blocks = spdk_bdev_get_optimal_io_boundary(bdev);
	uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint8_t sectors_per_block = blk_size >> LINUX_SECTOR_SHIFT;
	uint32_t io_min_size = blk_size;
	uint32_t io_opt_size = spdk_max(io_opt_blocks * blk_size, io_min_size);

	struct ublksrv_ctrl_dev_info uinfo = {
		.queue_depth = ublk->queue_depth,
		.nr_hw_queues = ublk->num_queues,
		.dev_id = ublk->ublk_id,
		.max_io_buf_bytes = UBLK_IO_MAX_BYTES,
		.ublksrv_pid = getpid(),
		.flags = UBLK_F_URING_CMD_COMP_IN_TASK,
	};
	struct ublk_params uparams = {
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift = spdk_u32log2(blk_size),
			.physical_bs_shift = spdk_u32log2(pblk_size),
			.io_min_shift = spdk_u32log2(io_min_size),
			.io_opt_shift = spdk_u32log2(io_opt_size),
			.dev_sectors = num_blocks * sectors_per_block,
			.max_sectors = UBLK_IO_MAX_BYTES >> LINUX_SECTOR_SHIFT,
		}
	};

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		uparams.types |= UBLK_PARAM_TYPE_DISCARD;
		uparams.discard.discard_alignment = sectors_per_block;
		uparams.discard.max_discard_sectors = num_blocks * sectors_per_block;
		uparams.discard.max_discard_segments = 1;
		uparams.discard.discard_granularity = blk_size;
		if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
			uparams.discard.max_write_zeroes_sectors = num_blocks * sectors_per_block;
		}
	}

	ublk->dev_info = uinfo;
	ublk->dev_params = uparams;
}

static int
ublk_ios_init(struct spdk_ublk_dev *ublk)
{
	char mempool_name[32];
	int rc;
	uint32_t i, j;
	struct ublk_queue *q;

	snprintf(mempool_name, sizeof(mempool_name), "ublk_io_buf_pool_%d", ublk->ublk_id);

	/* Create a mempool to allocate buf for each io */
	ublk->io_buf_pool = spdk_mempool_create(mempool_name,
						ublk->num_queues * ublk->queue_depth,
						UBLK_IO_MAX_BYTES,
						0,
						SPDK_ENV_SOCKET_ID_ANY);
	if (ublk->io_buf_pool == NULL) {
		rc = -ENOMEM;
		SPDK_ERRLOG("could not allocate ublk_io_buf pool\n");
		return rc;
	}

	for (i = 0; i < ublk->num_queues; i++) {
		q = &ublk->queues[i];

		TAILQ_INIT(&q->completed_io_list);
		TAILQ_INIT(&q->inflight_io_list);
		q->dev = ublk;
		q->q_id = i;
		q->q_depth = ublk->queue_depth;
		q->ios = calloc(q->q_depth, sizeof(struct ublk_io));
		if (!q->ios) {
			rc = -ENOMEM;
			SPDK_ERRLOG("could not allocate queue ios\n");
			goto err;
		}
		for (j = 0; j < q->q_depth; j++) {
			q->ios[j].q = q;
			q->ios[j].payload = spdk_mempool_get(ublk->io_buf_pool);
		}
	}

	return 0;

err:
	spdk_mempool_free(ublk->io_buf_pool);
	for (i = 0; i < ublk->num_queues; i++) {
		q = &ublk->queues[i];

		free(q->ios);
	}
	return rc;
}

static void
ublk_queue_run(void *arg1)
{
	struct ublk_queue	*q = arg1;
	struct spdk_ublk_dev *ublk = q->dev;
	struct ublk_thread_ctx *thread_ctx = q->thread_ctx;

	assert(spdk_get_thread() == thread_ctx->ublk_thread);
	/* Queues must be filled with IO in the io pthread */
	ublk_dev_queue_io_init(q);

	ublk->ch[q->q_id] = spdk_bdev_get_io_channel(ublk->bdev_desc);
	TAILQ_INSERT_TAIL(&thread_ctx->queue_list, q, tailq);
}

int
ublk_start_disk(const char *bdev_name, uint32_t ublk_id,
		uint32_t num_queues, uint32_t queue_depth)
{
	int			rc;
	uint32_t		q_id, i;
	struct spdk_bdev	*bdev;
	struct spdk_ublk_dev	*ublk = NULL;
	struct spdk_thread	*ublk_thread;
	char			buf[64];

	if (g_ublk_tgt.active == false) {
		SPDK_ERRLOG("NO ublk target exist\n");
		return -ENODEV;
	}

	ublk = ublk_dev_find_by_id(ublk_id);
	if (ublk != NULL) {
		SPDK_DEBUGLOG(ublk, "ublk id %d is in use.\n", ublk_id);
		return -EBUSY;
	}
	ublk = calloc(1, sizeof(*ublk));
	if (ublk == NULL) {
		return -ENOMEM;
	}
	ublk->cdev_fd = -1;

	rc = spdk_bdev_open_ext(bdev_name, true, ublk_bdev_event_cb, ublk, &ublk->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s, error=%d\n", bdev_name, rc);
		free(ublk);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(ublk->bdev_desc);
	ublk->bdev = bdev;

	ublk->q_deinit_num = 0;
	ublk->app_thread = spdk_get_thread();
	ublk->ublk_id = ublk_id;
	ublk->num_queues = num_queues;
	ublk->queue_depth = queue_depth;
	if (ublk->queue_depth > UBLK_DEV_MAX_QUEUE_DEPTH) {
		SPDK_WARNLOG("Set Queue depth %d of UBLK %d to maximum %d\n",
			     ublk->queue_depth, ublk->ublk_id, UBLK_DEV_MAX_QUEUE_DEPTH);
		ublk->queue_depth = UBLK_DEV_MAX_QUEUE_DEPTH;
	}
	if (ublk->num_queues > UBLK_DEV_MAX_QUEUES) {
		SPDK_WARNLOG("Set Queue num %d of UBLK %d to maximum %d\n",
			     ublk->num_queues, ublk->ublk_id, UBLK_DEV_MAX_QUEUES);
		ublk->num_queues = UBLK_DEV_MAX_QUEUES;
	}

	/* Add ublk_dev to the end of disk list */
	rc = ublk_dev_list_register(ublk);
	if (rc != 0) {
		goto err;
	}

	ublk_info_param_init(ublk);
	rc = ublk_ios_init(ublk);
	if (rc != 0) {
		goto err;
	}

	SPDK_INFOLOG(ublk, "Enabling kernel access to bdev %s via ublk %d\n",
		     bdev_name, ublk_id);
	rc = _ublk_start_disk(ublk);
	if (rc != 0) {
		goto err;
	}

	snprintf(buf, 64, "%s%d", UBLK_BLK_CDEV, ublk->ublk_id);
	ublk->cdev_fd = open(buf, O_RDWR);
	if (ublk->cdev_fd < 0) {
		rc = ublk->cdev_fd;
		SPDK_ERRLOG("can't open %s, rc %d\n", buf, rc);
		goto err;
	}

	for (q_id = 0; q_id < ublk->num_queues; q_id++) {
		rc = ublk_dev_queue_init(&ublk->queues[q_id]);
		if (rc) {
			for (i = 0; i < q_id; i++) {
				ublk_dev_queue_fini(&ublk->queues[i]);
			}
			goto err;
		}
	}

	rc = ublk_start_kernel(ublk);
	if (rc != 0) {
		goto err;
	}

	/* Send queue to different spdk_threads for load balance */
	for (q_id = 0; q_id < ublk->num_queues; q_id++) {
		ublk->queues[q_id].thread_ctx = &g_ublk_tgt.thread_ctx[g_queue_thread_id];
		ublk_thread = g_ublk_tgt.thread_ctx[g_queue_thread_id].ublk_thread;
		spdk_thread_send_msg(ublk_thread, ublk_queue_run, &ublk->queues[q_id]);
		g_queue_thread_id++;
		if (g_queue_thread_id == g_num_ublk_threads) {
			g_queue_thread_id = 0;
		}
	}

	return 0;

err:
	_ublk_try_close_dev(ublk);
	return rc;
}

SPDK_LOG_REGISTER_COMPONENT(ublk)
