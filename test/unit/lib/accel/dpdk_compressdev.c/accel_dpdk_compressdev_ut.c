/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
/* We have our own mock for this */
#define UNIT_TEST_NO_VTOPHYS
#include "common/lib/test_env.c"
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "unit/lib/json_mock.c"

#include <rte_compressdev.h>

/* There will be one if the data perfectly matches the chunk size,
 * or there could be an offset into the data and a remainder after
 * the data or both for a max of 3.
 */
#define UT_MBUFS_PER_OP 3
/* For testing the crossing of a huge page boundary on address translation,
 * we'll have an extra one but we only test on the source side.
 */
#define UT_MBUFS_PER_OP_BOUND_TEST 4

struct spdk_io_channel *g_io_ch;
struct rte_comp_op g_comp_op[2];
struct comp_device_qp g_device_qp;
struct compress_dev g_device;
struct rte_compressdev_capabilities g_cdev_cap;
static struct rte_mbuf *g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST];
static struct rte_mbuf *g_dst_mbufs[UT_MBUFS_PER_OP];
static struct rte_mbuf g_expected_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST];
static struct rte_mbuf g_expected_dst_mbufs[UT_MBUFS_PER_OP];
struct compress_io_channel *g_comp_ch;

/* Those functions are defined as static inline in DPDK, so we can't
 * mock them straight away. We use defines to redirect them into
 * our custom functions.
 */

static int ut_total_rte_pktmbuf_attach_extbuf = 0;
static void mock_rte_pktmbuf_attach_extbuf(struct rte_mbuf *m, void *buf_addr, rte_iova_t buf_iova,
		uint16_t buf_len, struct rte_mbuf_ext_shared_info *shinfo);
#define rte_pktmbuf_attach_extbuf mock_rte_pktmbuf_attach_extbuf
static void
mock_rte_pktmbuf_attach_extbuf(struct rte_mbuf *m, void *buf_addr, rte_iova_t buf_iova,
			       uint16_t buf_len, struct rte_mbuf_ext_shared_info *shinfo)
{
	assert(m != NULL);
	m->buf_addr = buf_addr;
	m->buf_iova = buf_iova;
	m->buf_len = buf_len;
	m->data_len = m->pkt_len = 0;
	ut_total_rte_pktmbuf_attach_extbuf++;
}

static char *mock_rte_pktmbuf_append(struct rte_mbuf *m, uint16_t len);
#define rte_pktmbuf_append mock_rte_pktmbuf_append
static char *
mock_rte_pktmbuf_append(struct rte_mbuf *m, uint16_t len)
{
	m->pkt_len = m->pkt_len + len;
	return NULL;
}

static inline int mock_rte_pktmbuf_chain(struct rte_mbuf *head, struct rte_mbuf *tail);
#define rte_pktmbuf_chain mock_rte_pktmbuf_chain
static inline int
mock_rte_pktmbuf_chain(struct rte_mbuf *head, struct rte_mbuf *tail)
{
	struct rte_mbuf *cur_tail;

	cur_tail = rte_pktmbuf_lastseg(head);
	cur_tail->next = tail;

	return 0;
}

uint16_t ut_max_nb_queue_pairs = 0;
void __rte_experimental mock_rte_compressdev_info_get(uint8_t dev_id,
		struct rte_compressdev_info *dev_info);
#define rte_compressdev_info_get mock_rte_compressdev_info_get
void __rte_experimental
mock_rte_compressdev_info_get(uint8_t dev_id, struct rte_compressdev_info *dev_info)
{
	dev_info->max_nb_queue_pairs = ut_max_nb_queue_pairs;
	dev_info->capabilities = &g_cdev_cap;
	dev_info->driver_name = "compressdev";
}

int ut_rte_compressdev_configure = 0;
int __rte_experimental mock_rte_compressdev_configure(uint8_t dev_id,
		struct rte_compressdev_config *config);
#define rte_compressdev_configure mock_rte_compressdev_configure
int __rte_experimental
mock_rte_compressdev_configure(uint8_t dev_id, struct rte_compressdev_config *config)
{
	return ut_rte_compressdev_configure;
}

int ut_rte_compressdev_queue_pair_setup = 0;
int __rte_experimental mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
		uint32_t max_inflight_ops, int socket_id);
#define rte_compressdev_queue_pair_setup mock_rte_compressdev_queue_pair_setup
int __rte_experimental
mock_rte_compressdev_queue_pair_setup(uint8_t dev_id, uint16_t queue_pair_id,
				      uint32_t max_inflight_ops, int socket_id)
{
	return ut_rte_compressdev_queue_pair_setup;
}

int ut_rte_compressdev_start = 0;
int __rte_experimental mock_rte_compressdev_start(uint8_t dev_id);
#define rte_compressdev_start mock_rte_compressdev_start
int __rte_experimental
mock_rte_compressdev_start(uint8_t dev_id)
{
	return ut_rte_compressdev_start;
}

int ut_rte_compressdev_private_xform_create = 0;
int __rte_experimental mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform);
#define rte_compressdev_private_xform_create mock_rte_compressdev_private_xform_create
int __rte_experimental
mock_rte_compressdev_private_xform_create(uint8_t dev_id,
		const struct rte_comp_xform *xform, void **private_xform)
{
	return ut_rte_compressdev_private_xform_create;
}

uint8_t ut_rte_compressdev_count = 0;
uint8_t __rte_experimental mock_rte_compressdev_count(void);
#define rte_compressdev_count mock_rte_compressdev_count
uint8_t __rte_experimental
mock_rte_compressdev_count(void)
{
	return ut_rte_compressdev_count;
}

struct rte_mempool *ut_rte_comp_op_pool_create = NULL;
struct rte_mempool *__rte_experimental mock_rte_comp_op_pool_create(const char *name,
		unsigned int nb_elts, unsigned int cache_size, uint16_t user_size,
		int socket_id);
#define rte_comp_op_pool_create mock_rte_comp_op_pool_create
struct rte_mempool *__rte_experimental
mock_rte_comp_op_pool_create(const char *name, unsigned int nb_elts,
			     unsigned int cache_size, uint16_t user_size, int socket_id)
{
	return ut_rte_comp_op_pool_create;
}

void mock_rte_pktmbuf_free(struct rte_mbuf *m);
#define rte_pktmbuf_free mock_rte_pktmbuf_free
void
mock_rte_pktmbuf_free(struct rte_mbuf *m)
{
}

void mock_rte_pktmbuf_free_bulk(struct rte_mbuf **m, unsigned int cnt);
#define rte_pktmbuf_free_bulk mock_rte_pktmbuf_free_bulk
void
mock_rte_pktmbuf_free_bulk(struct rte_mbuf **m, unsigned int cnt)
{
}

static bool ut_boundary_alloc = false;
static int ut_rte_pktmbuf_alloc_bulk = 0;
int mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
				unsigned count);
#define rte_pktmbuf_alloc_bulk mock_rte_pktmbuf_alloc_bulk
int
mock_rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs,
			    unsigned count)
{
	int i;

	/* This mocked function only supports the alloc of up to 3 src and 3 dst. */
	ut_rte_pktmbuf_alloc_bulk += count;

	if (ut_rte_pktmbuf_alloc_bulk == 1) {
		/* allocation of an extra mbuf for boundary cross test */
		ut_boundary_alloc = true;
		g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1]->next = NULL;
		*mbufs = g_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1];
		ut_rte_pktmbuf_alloc_bulk = 0;
	} else if (ut_rte_pktmbuf_alloc_bulk == UT_MBUFS_PER_OP) {
		/* first test allocation, src mbufs */
		for (i = 0; i < UT_MBUFS_PER_OP; i++) {
			g_src_mbufs[i]->next = NULL;
			*mbufs++ = g_src_mbufs[i];
		}
	} else if (ut_rte_pktmbuf_alloc_bulk == UT_MBUFS_PER_OP * 2) {
		/* second test allocation, dst mbufs */
		for (i = 0; i < UT_MBUFS_PER_OP; i++) {
			g_dst_mbufs[i]->next = NULL;
			*mbufs++ = g_dst_mbufs[i];
		}
		ut_rte_pktmbuf_alloc_bulk = 0;
	} else {
		return -1;
	}
	return 0;
}

struct rte_mempool *
rte_pktmbuf_pool_create(const char *name, unsigned n, unsigned cache_size,
			uint16_t priv_size, uint16_t data_room_size, int socket_id)
{
	struct spdk_mempool *tmp;

	tmp = spdk_mempool_create("mbuf_mp", 1024, sizeof(struct rte_mbuf),
				  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				  SPDK_ENV_SOCKET_ID_ANY);

	return (struct rte_mempool *)tmp;
}

void
rte_mempool_free(struct rte_mempool *mp)
{
	if (mp) {
		spdk_mempool_free((struct spdk_mempool *)mp);
	}
}

#include "accel/dpdk_compressdev/accel_dpdk_compressdev.c"

static void _compress_done(void *arg, int status);
static int ut_expected_task_status = 0;
void
spdk_accel_task_complete(struct spdk_accel_task *accel_task, int status)
{
	CU_ASSERT(status == ut_expected_task_status);
	accel_task->cb_fn(accel_task, status);
}

/* SPDK stubs */
DEFINE_STUB_V(spdk_accel_module_finish, (void));
DEFINE_STUB_V(spdk_accel_module_list_add, (struct spdk_accel_module_if *accel_module));

/* DPDK stubs */
DEFINE_STUB(rte_compressdev_capability_get, const struct rte_compressdev_capabilities *,
	    (uint8_t dev_id,
	     enum rte_comp_algorithm algo), NULL);
#define DPDK_DYNFIELD_OFFSET offsetof(struct rte_mbuf, dynfield1[1])
DEFINE_STUB(rte_mbuf_dynfield_register, int, (const struct rte_mbuf_dynfield *params),
	    DPDK_DYNFIELD_OFFSET);
DEFINE_STUB(rte_socket_id, unsigned, (void), 0);
DEFINE_STUB(rte_vdev_init, int, (const char *name, const char *args), 0);
DEFINE_STUB_V(rte_comp_op_free, (struct rte_comp_op *op));
DEFINE_STUB(rte_comp_op_alloc, struct rte_comp_op *, (struct rte_mempool *mempool), NULL);

int g_small_size_counter = 0;
int g_small_size_modify = 0;
uint64_t g_small_size = 0;
uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	g_small_size_counter++;
	if (g_small_size_counter == g_small_size_modify) {
		*size = g_small_size;
		g_small_size_counter = 0;
		g_small_size_modify = 0;
	}
	return (uint64_t)buf;
}

static uint16_t ut_rte_compressdev_dequeue_burst = 0;
uint16_t
rte_compressdev_dequeue_burst(uint8_t dev_id, uint16_t qp_id, struct rte_comp_op **ops,
			      uint16_t nb_op)
{
	if (ut_rte_compressdev_dequeue_burst == 0) {
		return 0;
	}

	ops[0] = &g_comp_op[0];
	ops[1] = &g_comp_op[1];

	return ut_rte_compressdev_dequeue_burst;
}

static uint16_t g_done_count = 1;
static void
_compress_done(void *arg, int status)
{
	struct spdk_accel_task *task  = arg;

	if (status == 0) {
		CU_ASSERT(*task->output_size == g_comp_op[g_done_count++].produced);
	}
}

static void
_get_mbuf_array(struct rte_mbuf **mbuf_array, struct rte_mbuf *mbuf_head,
		int mbuf_count, bool null_final)
{
	int i;

	for (i = 0; i < mbuf_count; i++) {
		mbuf_array[i] = mbuf_head;
		if (mbuf_head) {
			mbuf_head = mbuf_head->next;
		}
	}
	if (null_final) {
		mbuf_array[i - 1] = NULL;
	}
}

#define FAKE_ENQUEUE_SUCCESS 255
#define FAKE_ENQUEUE_ERROR 128
#define FAKE_ENQUEUE_BUSY 64
static uint16_t ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
static struct rte_comp_op ut_expected_op;
uint16_t
rte_compressdev_enqueue_burst(uint8_t dev_id, uint16_t qp_id, struct rte_comp_op **ops,
			      uint16_t nb_ops)
{
	struct rte_comp_op *op = *ops;
	struct rte_mbuf *op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	struct rte_mbuf *exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	int i, num_src_mbufs = UT_MBUFS_PER_OP;

	switch (ut_enqueue_value) {
	case FAKE_ENQUEUE_BUSY:
		op->status = RTE_COMP_OP_STATUS_NOT_PROCESSED;
		return 0;
	case FAKE_ENQUEUE_SUCCESS:
		op->status = RTE_COMP_OP_STATUS_SUCCESS;
		return 1;
	case FAKE_ENQUEUE_ERROR:
		op->status = RTE_COMP_OP_STATUS_ERROR;
		return 0;
	default:
		break;
	}

	/* by design the compress module will never send more than 1 op at a time */
	CU_ASSERT(op->private_xform == ut_expected_op.private_xform);

	/* setup our local pointers to the chained mbufs, those pointed to in the
	 * operation struct and the expected values.
	 */
	_get_mbuf_array(op_mbuf, op->m_src, SPDK_COUNTOF(op_mbuf), true);
	_get_mbuf_array(exp_mbuf, ut_expected_op.m_src, SPDK_COUNTOF(exp_mbuf), true);

	if (ut_boundary_alloc == true) {
		/* if we crossed a boundary, we need to check the 4th src mbuf and
		 * reset the global that is used to identify whether we crossed
		 * or not
		 */
		num_src_mbufs = UT_MBUFS_PER_OP_BOUND_TEST;
		exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] = ut_expected_op.m_src->next->next->next;
		op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] = op->m_src->next->next->next;
		ut_boundary_alloc = false;
	}

	for (i = 0; i < num_src_mbufs; i++) {
		CU_ASSERT(op_mbuf[i]->buf_addr == exp_mbuf[i]->buf_addr);
		CU_ASSERT(op_mbuf[i]->buf_iova == exp_mbuf[i]->buf_iova);
		CU_ASSERT(op_mbuf[i]->buf_len == exp_mbuf[i]->buf_len);
		CU_ASSERT(op_mbuf[i]->pkt_len == exp_mbuf[i]->pkt_len);
	}

	/* if only 3 mbufs were used in the test, the 4th should be zeroed */
	if (num_src_mbufs == UT_MBUFS_PER_OP) {
		CU_ASSERT(op_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] == NULL);
		CU_ASSERT(exp_mbuf[UT_MBUFS_PER_OP_BOUND_TEST - 1] == NULL);
	}
	CU_ASSERT(*RTE_MBUF_DYNFIELD(op->m_src, g_mbuf_offset, uint64_t *) ==
		  *RTE_MBUF_DYNFIELD(ut_expected_op.m_src, g_mbuf_offset, uint64_t *));
	CU_ASSERT(op->src.offset == ut_expected_op.src.offset);
	CU_ASSERT(op->src.length == ut_expected_op.src.length);

	/* check dst mbuf values */
	_get_mbuf_array(op_mbuf, op->m_dst, SPDK_COUNTOF(op_mbuf), true);
	_get_mbuf_array(exp_mbuf, ut_expected_op.m_dst, SPDK_COUNTOF(exp_mbuf), true);

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		CU_ASSERT(op_mbuf[i]->buf_addr == exp_mbuf[i]->buf_addr);
		CU_ASSERT(op_mbuf[i]->buf_iova == exp_mbuf[i]->buf_iova);
		CU_ASSERT(op_mbuf[i]->buf_len == exp_mbuf[i]->buf_len);
		CU_ASSERT(op_mbuf[i]->pkt_len == exp_mbuf[i]->pkt_len);
	}
	CU_ASSERT(op->dst.offset == ut_expected_op.dst.offset);

	return ut_enqueue_value;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	struct spdk_thread *thread;
	int i;

	spdk_thread_lib_init(NULL, 0);

	thread = spdk_thread_create(NULL, NULL);
	spdk_set_thread(thread);

	g_comp_xform = (struct rte_comp_xform) {
		.type = RTE_COMP_COMPRESS,
		.compress = {
			.algo = RTE_COMP_ALGO_DEFLATE,
			.deflate.huffman = RTE_COMP_HUFFMAN_DEFAULT,
			.level = RTE_COMP_LEVEL_MAX,
			.window_size = DEFAULT_WINDOW_SIZE,
			.chksum = RTE_COMP_CHECKSUM_NONE,
			.hash_algo = RTE_COMP_HASH_ALGO_NONE
		}
	};

	g_decomp_xform = (struct rte_comp_xform) {
		.type = RTE_COMP_DECOMPRESS,
		.decompress = {
			.algo = RTE_COMP_ALGO_DEFLATE,
			.chksum = RTE_COMP_CHECKSUM_NONE,
			.window_size = DEFAULT_WINDOW_SIZE,
			.hash_algo = RTE_COMP_HASH_ALGO_NONE
		}
	};
	g_device.comp_xform = &g_comp_xform;
	g_device.decomp_xform = &g_decomp_xform;
	g_cdev_cap.comp_feature_flags = RTE_COMP_FF_SHAREABLE_PRIV_XFORM;
	g_device.cdev_info.driver_name = "compressdev";
	g_device.cdev_info.capabilities = &g_cdev_cap;
	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST; i++) {
		g_src_mbufs[i] = spdk_zmalloc(sizeof(struct rte_mbuf), 0x40, NULL,
					      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	}
	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		g_dst_mbufs[i] = spdk_zmalloc(sizeof(struct rte_mbuf), 0x40, NULL,
					      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	}

	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct compress_io_channel));
	g_io_ch->thread = thread;
	g_comp_ch = (struct compress_io_channel *)spdk_io_channel_get_ctx(g_io_ch);
	g_comp_ch->device_qp = &g_device_qp;
	g_comp_ch->device_qp->device = &g_device;
	g_device_qp.device->sgl_in = true;
	g_device_qp.device->sgl_out = true;
	g_comp_ch->src_mbufs = calloc(UT_MBUFS_PER_OP_BOUND_TEST, sizeof(void *));
	g_comp_ch->dst_mbufs = calloc(UT_MBUFS_PER_OP, sizeof(void *));
	TAILQ_INIT(&g_comp_ch->queued_tasks);

	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST - 1; i++) {
		g_expected_src_mbufs[i].next = &g_expected_src_mbufs[i + 1];
	}
	g_expected_src_mbufs[UT_MBUFS_PER_OP_BOUND_TEST - 1].next = NULL;

	/* we only test w/4 mbufs on src side */
	for (i = 0; i < UT_MBUFS_PER_OP - 1; i++) {
		g_expected_dst_mbufs[i].next = &g_expected_dst_mbufs[i + 1];
	}
	g_expected_dst_mbufs[UT_MBUFS_PER_OP - 1].next = NULL;
	g_mbuf_offset = DPDK_DYNFIELD_OFFSET;

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	struct spdk_thread *thread;
	int i;

	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST; i++) {
		spdk_free(g_src_mbufs[i]);
	}
	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		spdk_free(g_dst_mbufs[i]);
	}
	free(g_comp_ch->src_mbufs);
	free(g_comp_ch->dst_mbufs);
	free(g_io_ch);

	thread = spdk_get_thread();
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	spdk_thread_lib_fini();

	return 0;
}

static void
test_compress_operation(void)
{
	struct iovec src_iovs[3] = {};
	int src_iovcnt;
	struct iovec dst_iovs[3] = {};
	int dst_iovcnt;
	struct spdk_accel_task *task_p, task = {};
	int rc, i;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP];
	struct rte_mbuf *exp_dst_mbuf[UT_MBUFS_PER_OP];
	uint32_t output_size;

	src_iovcnt = dst_iovcnt = 3;
	for (i = 0; i < dst_iovcnt; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}

	task.cb_fn = _compress_done;
	task.op_code = ACCEL_OPC_COMPRESS;
	task.output_size = &output_size;
	task.d.iovs = dst_iovs;
	task.d.iovcnt = dst_iovcnt;
	task.s.iovs = src_iovs;
	task.s.iovcnt = src_iovcnt;
	task_p = &task;

	/* test rte_comp_op_alloc failure */
	MOCK_SET(rte_comp_op_alloc, NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == false);
	while (!TAILQ_EMPTY(&g_comp_ch->queued_tasks)) {
		task_p = TAILQ_FIRST(&g_comp_ch->queued_tasks);
		TAILQ_REMOVE(&g_comp_ch->queued_tasks, task_p, link);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);

	/* test mempool get failure */
	MOCK_SET(rte_comp_op_alloc, &g_comp_op[0]);
	ut_rte_pktmbuf_alloc_bulk = -1;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == false);
	while (!TAILQ_EMPTY(&g_comp_ch->queued_tasks)) {
		task_p = TAILQ_FIRST(&g_comp_ch->queued_tasks);
		TAILQ_REMOVE(&g_comp_ch->queued_tasks, task_p, link);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);
	ut_rte_pktmbuf_alloc_bulk = 0;

	/* test enqueue failure busy */
	ut_enqueue_value = FAKE_ENQUEUE_BUSY;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == false);
	while (!TAILQ_EMPTY(&g_comp_ch->queued_tasks)) {
		task_p = TAILQ_FIRST(&g_comp_ch->queued_tasks);
		TAILQ_REMOVE(&g_comp_ch->queued_tasks, task_p, link);
	}
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);
	ut_enqueue_value = 1;

	/* test enqueue failure error */
	ut_enqueue_value = FAKE_ENQUEUE_ERROR;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == -EINVAL);
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;

	/* test success with 3 vector iovec */
	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len + src_iovs[2].iov_len;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);
	ut_expected_op.m_src = exp_src_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		*RTE_MBUF_DYNFIELD(exp_src_mbuf[i], g_mbuf_offset, uint64_t *) = (uint64_t)&task;
		exp_src_mbuf[i]->buf_addr = src_iovs[i].iov_base;
		exp_src_mbuf[i]->buf_iova = spdk_vtophys(src_iovs[i].iov_base, &src_iovs[i].iov_len);
		exp_src_mbuf[i]->buf_len = src_iovs[i].iov_len;
		exp_src_mbuf[i]->pkt_len = src_iovs[i].iov_len;
	}

	/* setup the dst expected values */
	_get_mbuf_array(exp_dst_mbuf, &g_expected_dst_mbufs[0], SPDK_COUNTOF(exp_dst_mbuf), false);
	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = exp_dst_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		exp_dst_mbuf[i]->buf_addr = dst_iovs[i].iov_base;
		exp_dst_mbuf[i]->buf_iova = spdk_vtophys(dst_iovs[i].iov_base, &dst_iovs[i].iov_len);
		exp_dst_mbuf[i]->buf_len = dst_iovs[i].iov_len;
		exp_dst_mbuf[i]->pkt_len = dst_iovs[i].iov_len;
	}

	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);

	/* test sgl out failure */
	g_device.sgl_out = false;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	g_device.sgl_out = true;

	/* test sgl in failure */
	g_device.sgl_in = false;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	g_device.sgl_in = true;
}

static void
test_compress_operation_cross_boundary(void)
{
	struct iovec src_iovs[3] = {};
	int src_iovcnt;
	struct iovec dst_iovs[3] = {};
	int dst_iovcnt;
	int rc, i;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	struct rte_mbuf *exp_dst_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	struct spdk_accel_task task = {};
	uint32_t output_size;

	/* Setup the same basic 3 IOV test as used in the simple success case
	 * but then we'll start testing a vtophy boundary crossing at each
	 * position.
	 */
	src_iovcnt = dst_iovcnt = 3;
	for (i = 0; i < dst_iovcnt; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}

	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len + src_iovs[2].iov_len;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);
	ut_expected_op.m_src = exp_src_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		*RTE_MBUF_DYNFIELD(exp_src_mbuf[i], g_mbuf_offset, uint64_t *) = (uint64_t)&task;
		exp_src_mbuf[i]->buf_addr = src_iovs[i].iov_base;
		exp_src_mbuf[i]->buf_iova = spdk_vtophys(src_iovs[i].iov_base, &src_iovs[i].iov_len);
		exp_src_mbuf[i]->buf_len = src_iovs[i].iov_len;
		exp_src_mbuf[i]->pkt_len = src_iovs[i].iov_len;
	}

	/* setup the dst expected values, we don't test needing a 4th dst mbuf */
	_get_mbuf_array(exp_dst_mbuf, &g_expected_dst_mbufs[0], SPDK_COUNTOF(exp_dst_mbuf), false);
	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = exp_dst_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		exp_dst_mbuf[i]->buf_addr = dst_iovs[i].iov_base;
		exp_dst_mbuf[i]->buf_iova = spdk_vtophys(dst_iovs[i].iov_base, &dst_iovs[i].iov_len);
		exp_dst_mbuf[i]->buf_len = dst_iovs[i].iov_len;
		exp_dst_mbuf[i]->pkt_len = dst_iovs[i].iov_len;
	}

	/* force the 1st IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 1;
	g_small_size = 0x800;
	*RTE_MBUF_DYNFIELD(exp_src_mbuf[3], g_mbuf_offset, uint64_t *) = (uint64_t)&task;

	/* first only has shorter length */
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x800;

	/* 2nd was inserted by the boundary crossing condition and finishes off
	 * the length from the first */
	exp_src_mbuf[1]->buf_addr = (void *)0x10000800;
	exp_src_mbuf[1]->buf_iova = 0x10000800;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x800;

	/* 3rd looks like that the 2nd would have */
	exp_src_mbuf[2]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[2]->buf_iova = 0x10001000;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x1000;

	/* a new 4th looks like what the 3rd would have */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[3]->buf_iova = 0x10002000;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x1000;

	task.cb_fn = _compress_done;
	task.op_code = ACCEL_OPC_COMPRESS;
	task.output_size = &output_size;
	task.d.iovs = dst_iovs;
	task.d.iovcnt = dst_iovcnt;
	task.s.iovs = src_iovs;
	task.s.iovcnt = src_iovcnt;

	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);

	/* Now force the 2nd IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 2;
	g_small_size = 0x800;

	/* first is normal */
	exp_src_mbuf[0]->buf_addr = (void *)0x10000000;
	exp_src_mbuf[0]->buf_iova = 0x10000000;
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x1000;

	/* second only has shorter length */
	exp_src_mbuf[1]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[1]->buf_iova = 0x10001000;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x800;

	/* 3rd was inserted by the boundary crossing condition and finishes off
	 * the length from the first */
	exp_src_mbuf[2]->buf_addr = (void *)0x10001800;
	exp_src_mbuf[2]->buf_iova = 0x10001800;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x800;

	/* a new 4th looks like what the 3rd would have */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[3]->buf_iova = 0x10002000;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x1000;

	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);

	/* Finally force the 3rd IOV to get partial length from spdk_vtophys */
	g_small_size_counter = 0;
	g_small_size_modify = 3;
	g_small_size = 0x800;

	/* first is normal */
	exp_src_mbuf[0]->buf_addr = (void *)0x10000000;
	exp_src_mbuf[0]->buf_iova = 0x10000000;
	exp_src_mbuf[0]->pkt_len = exp_src_mbuf[0]->buf_len = 0x1000;

	/* second is normal */
	exp_src_mbuf[1]->buf_addr = (void *)0x10001000;
	exp_src_mbuf[1]->buf_iova = 0x10001000;
	exp_src_mbuf[1]->pkt_len = exp_src_mbuf[1]->buf_len = 0x1000;

	/* 3rd has shorter length */
	exp_src_mbuf[2]->buf_addr = (void *)0x10002000;
	exp_src_mbuf[2]->buf_iova = 0x10002000;
	exp_src_mbuf[2]->pkt_len = exp_src_mbuf[2]->buf_len = 0x800;

	/* a new 4th handles the remainder from the 3rd */
	exp_src_mbuf[3]->buf_addr = (void *)0x10002800;
	exp_src_mbuf[3]->buf_iova = 0x10002800;
	exp_src_mbuf[3]->pkt_len = exp_src_mbuf[3]->buf_len = 0x800;

	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == 0);

	/* Single input iov is split on page boundary, sgl_in is not supported */
	g_device.sgl_in = false;
	g_small_size_counter = 0;
	g_small_size_modify = 1;
	g_small_size = 0x800;
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(rc == -EINVAL);
	g_device.sgl_in = true;

	/* Single output iov is split on page boundary, sgl_out is not supported */
	g_device.sgl_out = false;
	g_small_size_counter = 0;
	g_small_size_modify = 2;
	g_small_size = 0x800;
	rc = _compress_operation(g_comp_ch, &task);
	CU_ASSERT(rc == -EINVAL);
	g_device.sgl_out = true;
}

static void
test_setup_compress_mbuf(void)
{
	struct iovec src_iovs = {};
	int src_iovcnt = 1;
	struct spdk_accel_task task = {};
	int src_mbuf_added = 0;
	uint64_t total_length;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP_BOUND_TEST];
	int rc, i;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);

	/* no splitting */
	total_length = 0;
	ut_total_rte_pktmbuf_attach_extbuf = 0;
	src_iovs.iov_len = 0x1000;
	src_iovs.iov_base = (void *)0x10000000 + 0x1000;
	rc = _setup_compress_mbuf(exp_src_mbuf, &src_mbuf_added, &total_length,
				  &src_iovs, src_iovcnt, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(total_length = src_iovs.iov_len);
	CU_ASSERT(src_mbuf_added == 0);
	CU_ASSERT(ut_total_rte_pktmbuf_attach_extbuf == 1);

	/* one split, for splitting tests we need the global mbuf array unlinked,
	 * otherwise the functional code will attempt to link them but if they are
	 * already linked, it will just create a chain that links to itself */
	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST - 1; i++) {
		g_expected_src_mbufs[i].next = NULL;
	}
	total_length = 0;
	ut_total_rte_pktmbuf_attach_extbuf = 0;
	src_iovs.iov_len = 0x1000 + MBUF_SPLIT;
	exp_src_mbuf[0]->buf_len = src_iovs.iov_len;
	exp_src_mbuf[0]->pkt_len = src_iovs.iov_len;
	rc = _setup_compress_mbuf(exp_src_mbuf, &src_mbuf_added, &total_length,
				  &src_iovs, src_iovcnt, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(total_length = src_iovs.iov_len);
	CU_ASSERT(src_mbuf_added == 0);
	CU_ASSERT(ut_total_rte_pktmbuf_attach_extbuf == 2);

	/* two splits */
	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST - 1; i++) {
		g_expected_src_mbufs[i].next = NULL;
	}
	total_length = 0;
	ut_total_rte_pktmbuf_attach_extbuf = 0;
	src_iovs.iov_len = 0x1000 + 2 * MBUF_SPLIT;
	exp_src_mbuf[0]->buf_len = src_iovs.iov_len;
	exp_src_mbuf[0]->pkt_len = src_iovs.iov_len;

	rc = _setup_compress_mbuf(exp_src_mbuf, &src_mbuf_added, &total_length,
				  &src_iovs, src_iovcnt, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(total_length = src_iovs.iov_len);
	CU_ASSERT(src_mbuf_added == 0);
	CU_ASSERT(ut_total_rte_pktmbuf_attach_extbuf == 3);

	/* relink the global mbuf array */
	for (i = 0; i < UT_MBUFS_PER_OP_BOUND_TEST - 1; i++) {
		g_expected_src_mbufs[i].next = &g_expected_src_mbufs[i + 1];
	}
}

static void
test_poller(void)
{
	int rc;
	struct compress_io_channel *args;
	struct rte_mbuf mbuf[4]; /* one src, one dst, 2 ops */
	struct iovec src_iovs[3] = {};
	struct iovec dst_iovs[3] = {};
	uint32_t output_size[2];
	struct spdk_accel_task task[2] = {};
	struct spdk_accel_task *task_to_resubmit;
	struct rte_mbuf *exp_src_mbuf[UT_MBUFS_PER_OP];
	struct rte_mbuf *exp_dst_mbuf[UT_MBUFS_PER_OP];
	int i;

	args = calloc(1, sizeof(*args));
	SPDK_CU_ASSERT_FATAL(args != NULL);
	memset(&g_comp_op[0], 0, sizeof(struct rte_comp_op));
	g_comp_op[0].m_src = &mbuf[0];
	g_comp_op[1].m_src = &mbuf[1];
	g_comp_op[0].m_dst = &mbuf[2];
	g_comp_op[1].m_dst = &mbuf[3];
	for (i = 0; i < 3; i++) {
		src_iovs[i].iov_len = 0x1000;
		dst_iovs[i].iov_len = 0x1000;
		src_iovs[i].iov_base = (void *)0x10000000 + 0x1000 * i;
		dst_iovs[i].iov_base = (void *)0x20000000 + 0x1000 * i;
	}
	task[0].cb_fn = task[1].cb_fn = _compress_done;
	task[0].output_size = &output_size[0];
	task[1].output_size = &output_size[1];

	/* Error from dequeue, nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 1;
	ut_expected_task_status = RTE_COMP_OP_STATUS_NOT_PROCESSED;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)&task[0];
	g_comp_op[0].produced = 1;
	g_done_count = 0;
	g_comp_op[0].status = RTE_COMP_OP_STATUS_NOT_PROCESSED;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = comp_dev_poller((void *)g_comp_ch);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);
	ut_expected_task_status = RTE_COMP_OP_STATUS_SUCCESS;

	/* Success from dequeue, 2 ops. nothing needing to be resubmitted.
	 */
	ut_rte_compressdev_dequeue_burst = 2;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)&task[0];
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = RTE_COMP_OP_STATUS_SUCCESS;
	*RTE_MBUF_DYNFIELD(g_comp_op[1].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)&task[1];
	g_comp_op[1].produced = 32;
	g_comp_op[1].status = RTE_COMP_OP_STATUS_SUCCESS;
	g_done_count = 0;
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	rc = comp_dev_poller((void *)g_comp_ch);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);

	/* One to dequeue, one op to be resubmitted. */
	ut_rte_compressdev_dequeue_burst = 1;
	/* setup what we want dequeue to return for the op */
	*RTE_MBUF_DYNFIELD(g_comp_op[0].m_src, g_mbuf_offset, uint64_t *) = (uint64_t)&task[0];
	g_comp_op[0].produced = 16;
	g_comp_op[0].status = 0;
	g_done_count = 0;
	task_to_resubmit = calloc(1, sizeof(struct spdk_accel_task));
	SPDK_CU_ASSERT_FATAL(task_to_resubmit != NULL);
	task_to_resubmit->s.iovs = &src_iovs[0];
	task_to_resubmit->s.iovcnt = 3;
	task_to_resubmit->d.iovs = &dst_iovs[0];
	task_to_resubmit->d.iovcnt = 3;
	task_to_resubmit->op_code = ACCEL_OPC_COMPRESS;
	task_to_resubmit->cb_arg = args;
	ut_enqueue_value = FAKE_ENQUEUE_SUCCESS;
	ut_expected_op.private_xform = &g_decomp_xform;
	ut_expected_op.src.offset = 0;
	ut_expected_op.src.length = src_iovs[0].iov_len + src_iovs[1].iov_len + src_iovs[2].iov_len;

	/* setup the src expected values */
	_get_mbuf_array(exp_src_mbuf, &g_expected_src_mbufs[0], SPDK_COUNTOF(exp_src_mbuf), false);
	ut_expected_op.m_src = exp_src_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		*RTE_MBUF_DYNFIELD(exp_src_mbuf[i], g_mbuf_offset, uint64_t *) = (uint64_t)&task[0];
		exp_src_mbuf[i]->buf_addr = src_iovs[i].iov_base;
		exp_src_mbuf[i]->buf_iova = spdk_vtophys(src_iovs[i].iov_base, &src_iovs[i].iov_len);
		exp_src_mbuf[i]->buf_len = src_iovs[i].iov_len;
		exp_src_mbuf[i]->pkt_len = src_iovs[i].iov_len;
	}

	/* setup the dst expected values */
	_get_mbuf_array(exp_dst_mbuf, &g_expected_dst_mbufs[0], SPDK_COUNTOF(exp_dst_mbuf), false);
	ut_expected_op.dst.offset = 0;
	ut_expected_op.m_dst = exp_dst_mbuf[0];

	for (i = 0; i < UT_MBUFS_PER_OP; i++) {
		exp_dst_mbuf[i]->buf_addr = dst_iovs[i].iov_base;
		exp_dst_mbuf[i]->buf_iova = spdk_vtophys(dst_iovs[i].iov_base, &dst_iovs[i].iov_len);
		exp_dst_mbuf[i]->buf_len = dst_iovs[i].iov_len;
		exp_dst_mbuf[i]->pkt_len = dst_iovs[i].iov_len;
	}
	MOCK_SET(rte_comp_op_alloc, &g_comp_op[0]);
	TAILQ_INSERT_TAIL(&g_comp_ch->queued_tasks,
			  task_to_resubmit,
			  link);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == false);
	rc = comp_dev_poller((void *)g_comp_ch);
	CU_ASSERT(TAILQ_EMPTY(&g_comp_ch->queued_tasks) == true);
	CU_ASSERT(rc == SPDK_POLLER_BUSY);

	free(task_to_resubmit);
	free(args);
}

static void
test_initdrivers(void)
{
	int rc;

	/* compressdev count 0 */
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == 0);

	/* bogus count */
	ut_rte_compressdev_count = RTE_COMPRESS_MAX_DEVS + 1;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);

	/* failure with rte_mbuf_dynfield_register */
	ut_rte_compressdev_count = 1;
	MOCK_SET(rte_mbuf_dynfield_register, -1);
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);
	MOCK_SET(rte_mbuf_dynfield_register, DPDK_DYNFIELD_OFFSET);

	/* error on create_compress_dev() */
	ut_rte_comp_op_pool_create = (struct rte_mempool *)0xDEADBEEF;
	ut_rte_compressdev_count = 1;
	ut_rte_compressdev_configure = -1;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* error on create_compress_dev() but coverage for large num queues */
	ut_max_nb_queue_pairs = 99;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* qpair setup fails */
	ut_rte_compressdev_configure = 0;
	ut_max_nb_queue_pairs = 0;
	ut_rte_compressdev_queue_pair_setup = -1;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -EINVAL);

	/* rte_compressdev_start fails */
	ut_rte_compressdev_queue_pair_setup = 0;
	ut_rte_compressdev_start = -1;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -1);

	/* rte_compressdev_private_xform_create() fails */
	ut_rte_compressdev_start = 0;
	ut_rte_compressdev_private_xform_create = -2;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == -2);

	/* success */
	ut_rte_compressdev_private_xform_create = 0;
	rc = accel_init_compress_drivers();
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("compress", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_compress_operation);
	CU_ADD_TEST(suite, test_compress_operation_cross_boundary);
	CU_ADD_TEST(suite, test_setup_compress_mbuf);
	CU_ADD_TEST(suite, test_initdrivers);
	CU_ADD_TEST(suite, test_poller);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
