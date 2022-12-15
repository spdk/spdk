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

#define UBLK_CTRL_RING_DEPTH		32
#define UBLK_THREAD_MAX			128

static uint32_t g_num_ublk_threads = 0;
static struct spdk_cpuset g_core_mask;

struct ublk_tgt {
	int			ctrl_fd;
	bool			active;
	bool			is_destroying;
	spdk_ublk_fini_cb	cb_fn;
	void			*cb_arg;
	struct io_uring		ctrl_ring;
	struct spdk_thread	*ublk_threads[UBLK_THREAD_MAX];
};

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

int
ublk_create_target(const char *cpumask_str)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct spdk_cpuset cpuset = {};
	struct spdk_cpuset thd_cpuset = {};

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
			g_ublk_tgt.ublk_threads[g_num_ublk_threads] = spdk_thread_create(thread_name, &thd_cpuset);
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
		if (g_ublk_tgt.ublk_threads[i] == ublk_thread) {
			spdk_thread_exit(ublk_thread);
		}
	}
}

/* This function will be used and extended in next patch */
static void
_ublk_fini(void *args)
{
	spdk_for_each_thread(ublk_thread_exit, NULL, _ublk_fini_done);
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

void
spdk_ublk_write_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_array_begin(w);

	spdk_json_write_array_end(w);
}
