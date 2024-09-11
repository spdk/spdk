/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/cunit.h"

#include "CUnit/Basic.h"

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;

#define WORKER_COUNT 2
#define WORKER_IO 0
#define WORKER_UT 1

static struct spdk_thread *g_thread[WORKER_COUNT];
static int g_num_failures = 0;
static bool g_shutdown = false;
static bool g_completion_success;
struct spdk_io_channel	*g_channel = NULL;

struct dif_task {
	struct iovec		*dst_iovs;
	uint32_t		dst_iovcnt;
	struct iovec		*src_iovs;
	uint32_t		src_iovcnt;
	struct iovec		*aux_iovs;
	uint32_t		aux_iovcnt;
	struct iovec		md_iov;
	uint32_t		num_blocks; /* used for the DIF related operations */
	struct spdk_dif_ctx	dif_ctx;
	struct spdk_dif_error	dif_err;
};

static void
execute_spdk_function(spdk_msg_fn fn, void *arg)
{
	pthread_mutex_lock(&g_test_mutex);
	spdk_thread_send_msg(g_thread[WORKER_IO], fn, arg);
	pthread_cond_wait(&g_test_cond, &g_test_mutex);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
wake_ut_thread(void)
{
	pthread_mutex_lock(&g_test_mutex);
	pthread_cond_signal(&g_test_cond);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
exit_io_thread(void *arg)
{
	assert(spdk_get_thread() == g_thread[WORKER_IO]);
	spdk_thread_exit(g_thread[WORKER_IO]);
	wake_ut_thread();
}

#define DATA_PATTERN 0x5A

static int g_xfer_size_bytes = 4096;
static int g_block_size_bytes = 512;
static int g_md_size_bytes = 8;
struct dif_task g_dif_task;

struct accel_dif_request {
	struct spdk_accel_sequence *sequence;
	struct spdk_io_channel *channel;
	struct iovec *dst_iovs;
	size_t dst_iovcnt;
	struct iovec *src_iovs;
	size_t src_iovcnt;
	struct iovec *aux_iovs;
	size_t aux_iovcnt;
	struct iovec *md_iov;
	uint32_t num_blocks;
	const struct spdk_dif_ctx *ctx;
	struct spdk_dif_error *err;
	spdk_accel_completion_cb cb_fn;
	void *cb_arg;
};

static void
accel_dif_oper_done(void *arg1, int status)
{
	if (status == 0) {
		g_completion_success = true;
	}
	wake_ut_thread();
}

static bool
accel_dif_error_validate(const uint32_t dif_flags,
			 const struct spdk_dif_error *err)
{
	if (dif_flags & SPDK_DIF_FLAGS_GUARD_CHECK) {
		return err->err_type == SPDK_DIF_GUARD_ERROR;
	} else if (dif_flags & SPDK_DIF_FLAGS_APPTAG_CHECK) {
		return err->err_type == SPDK_DIF_APPTAG_ERROR;
	} else if (dif_flags & SPDK_DIF_FLAGS_REFTAG_CHECK) {
		return err->err_type == SPDK_DIF_REFTAG_ERROR;
	}

	return false;
}

static int
alloc_dif_verify_bufs(struct dif_task *task, uint32_t chained_count)
{
	int src_buff_len = g_xfer_size_bytes;
	uint32_t i = 0;

	assert(chained_count > 0);
	task->src_iovcnt = chained_count;
	task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->src_iovs == NULL)) {
		return -ENOMEM;
	}

	src_buff_len += (g_xfer_size_bytes / g_block_size_bytes) * g_md_size_bytes;

	for (i = 0; i < task->src_iovcnt; i++) {
		task->src_iovs[i].iov_base = spdk_dma_zmalloc(src_buff_len, 0, NULL);
		if (spdk_unlikely(task->src_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->src_iovs[i].iov_base, DATA_PATTERN, src_buff_len);
		task->src_iovs[i].iov_len = src_buff_len;
	}

	task->num_blocks = (g_xfer_size_bytes * chained_count) / g_block_size_bytes;

	return 0;
}

static int
alloc_dix_bufs(struct dif_task *task, uint32_t chained_count)
{
	int src_buff_len = g_xfer_size_bytes, md_buff_len;
	uint32_t i = 0;

	assert(chained_count > 0);
	task->src_iovcnt = chained_count;
	task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->src_iovs == NULL)) {
		return -ENOMEM;
	}

	md_buff_len = (g_xfer_size_bytes / g_block_size_bytes) * g_md_size_bytes * chained_count;

	for (i = 0; i < task->src_iovcnt; i++) {
		task->src_iovs[i].iov_base = spdk_dma_zmalloc(src_buff_len, 0, NULL);
		if (spdk_unlikely(task->src_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->src_iovs[i].iov_base, DATA_PATTERN, src_buff_len);
		task->src_iovs[i].iov_len = src_buff_len;
	}

	task->md_iov.iov_base = spdk_dma_zmalloc(md_buff_len, 0, NULL);
	if (spdk_unlikely(task->md_iov.iov_base == NULL)) {
		return -ENOMEM;
	}

	task->md_iov.iov_len = md_buff_len;
	task->num_blocks = (g_xfer_size_bytes * chained_count) / g_block_size_bytes;

	return 0;
}

static void
free_dif_verify_bufs(struct dif_task *task)
{
	uint32_t i = 0;

	if (task->src_iovs != NULL) {
		for (i = 0; i < task->src_iovcnt; i++) {
			if (task->src_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->src_iovs[i].iov_base);
			}
		}
		free(task->src_iovs);
	}
}

static void
free_dix_bufs(struct dif_task *task)
{
	uint32_t i = 0;

	if (task->src_iovs != NULL) {
		for (i = 0; i < task->src_iovcnt; i++) {
			if (task->src_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->src_iovs[i].iov_base);
			}
		}
		free(task->src_iovs);
	}

	if (task->md_iov.iov_base != NULL) {
		spdk_dma_free(task->md_iov.iov_base);
	}
}

static int
alloc_dif_verify_copy_bufs(struct dif_task *task, uint32_t chained_count)
{
	int dst_buff_len = g_xfer_size_bytes;
	uint32_t data_size_with_md;
	uint32_t i = 0;

	assert(chained_count > 0);
	task->src_iovcnt = chained_count;
	task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->src_iovs == NULL)) {
		return -ENOMEM;
	}

	task->num_blocks = g_xfer_size_bytes / g_block_size_bytes;

	/* Add bytes for each block for metadata */
	data_size_with_md = g_xfer_size_bytes + (task->num_blocks * g_md_size_bytes);

	for (i = 0; i < task->src_iovcnt; i++) {
		task->src_iovs[i].iov_base = spdk_dma_zmalloc(data_size_with_md, 0, NULL);
		if (spdk_unlikely(task->src_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->src_iovs[i].iov_base, DATA_PATTERN, data_size_with_md);
		task->src_iovs[i].iov_len = data_size_with_md;
	}

	task->dst_iovcnt = chained_count;
	task->dst_iovs = calloc(task->dst_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->dst_iovs == NULL)) {
		return -ENOMEM;
	}

	for (i = 0; i < task->dst_iovcnt; i++) {
		task->dst_iovs[i].iov_base = spdk_dma_zmalloc(dst_buff_len, 0, NULL);
		if (spdk_unlikely(task->dst_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->dst_iovs[i].iov_base, 0, dst_buff_len);
		task->dst_iovs[i].iov_len = dst_buff_len;
	}

	return 0;
}

static void
free_dif_verify_copy_bufs(struct dif_task *task)
{
	uint32_t i = 0;

	if (task->dst_iovs != NULL) {
		for (i = 0; i < task->dst_iovcnt; i++) {
			if (task->dst_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->dst_iovs[i].iov_base);
			}
		}
		free(task->dst_iovs);
	}

	if (task->src_iovs != NULL) {
		for (i = 0; i < task->src_iovcnt; i++) {
			if (task->src_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->src_iovs[i].iov_base);
			}
		}
		free(task->src_iovs);
	}
}

static int
alloc_dif_generate_copy_bufs(struct dif_task *task, uint32_t chained_count)
{
	int src_buff_len = g_xfer_size_bytes;
	uint32_t transfer_size_with_md;
	uint32_t i = 0;

	assert(chained_count > 0);
	task->dst_iovcnt = chained_count;
	task->dst_iovs = calloc(task->dst_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->dst_iovs == NULL)) {
		return -ENOMEM;
	}

	task->num_blocks = g_xfer_size_bytes / g_block_size_bytes;

	/* Add bytes for each block for metadata */
	transfer_size_with_md = g_xfer_size_bytes + (task->num_blocks * g_md_size_bytes);

	for (i = 0; i < task->dst_iovcnt; i++) {
		task->dst_iovs[i].iov_base = spdk_dma_zmalloc(transfer_size_with_md, 0, NULL);
		if (spdk_unlikely(task->dst_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->dst_iovs[i].iov_base, 0, transfer_size_with_md);
		task->dst_iovs[i].iov_len = transfer_size_with_md;
	}

	task->src_iovcnt = chained_count;
	task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->src_iovs == NULL)) {
		return -ENOMEM;
	}

	for (i = 0; i < task->src_iovcnt; i++) {
		task->src_iovs[i].iov_base = spdk_dma_zmalloc(src_buff_len, 0, NULL);
		if (spdk_unlikely(task->src_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->src_iovs[i].iov_base, DATA_PATTERN, src_buff_len);
		task->src_iovs[i].iov_len = src_buff_len;
	}

	return 0;
}

static void
free_dif_generate_copy_bufs(struct dif_task *task)
{
	uint32_t i = 0;

	if (task->dst_iovs != NULL) {
		for (i = 0; i < task->dst_iovcnt; i++) {
			if (task->dst_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->dst_iovs[i].iov_base);
			}
		}
		free(task->dst_iovs);
	}

	if (task->src_iovs != NULL) {
		for (i = 0; i < task->src_iovcnt; i++) {
			if (task->src_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->src_iovs[i].iov_base);
			}
		}
		free(task->src_iovs);
	}
}

static int
alloc_dif_generate_copy_sequence_bufs(struct dif_task *task, uint32_t chained_count)
{
	int src_buff_len = g_xfer_size_bytes;
	uint32_t transfer_size_with_md;
	uint32_t i = 0;

	assert(chained_count > 0);
	task->dst_iovcnt = chained_count;
	task->dst_iovs = calloc(task->dst_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->dst_iovs == NULL)) {
		return -ENOMEM;
	}

	task->num_blocks = g_xfer_size_bytes / g_block_size_bytes;

	/* Add bytes for each block for metadata */
	transfer_size_with_md = g_xfer_size_bytes + (task->num_blocks * g_md_size_bytes);

	for (i = 0; i < task->dst_iovcnt; i++) {
		task->dst_iovs[i].iov_base = spdk_dma_zmalloc(transfer_size_with_md, 0, NULL);
		if (spdk_unlikely(task->dst_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->dst_iovs[i].iov_base, 0, transfer_size_with_md);
		task->dst_iovs[i].iov_len = transfer_size_with_md;
	}

	task->src_iovcnt = chained_count;
	task->src_iovs = calloc(task->src_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->src_iovs == NULL)) {
		return -ENOMEM;
	}

	for (i = 0; i < task->src_iovcnt; i++) {
		task->src_iovs[i].iov_base = spdk_dma_zmalloc(src_buff_len, 0, NULL);
		if (spdk_unlikely(task->src_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->src_iovs[i].iov_base, DATA_PATTERN, src_buff_len);
		task->src_iovs[i].iov_len = src_buff_len;
	}

	/*
	 * For write, we do not want to insert DIF in place because host does not expect
	 * write buffer to be updated. We allocate auxiliary buffer to simulate such case.
	 */
	task->aux_iovcnt = chained_count;
	task->aux_iovs = calloc(task->aux_iovcnt, sizeof(struct iovec));
	if (spdk_unlikely(task->aux_iovs == NULL)) {
		return -ENOMEM;
	}

	for (i = 0; i < task->aux_iovcnt; i++) {
		task->aux_iovs[i].iov_base = spdk_dma_zmalloc(transfer_size_with_md, 0, NULL);
		if (spdk_unlikely(task->aux_iovs[i].iov_base == NULL)) {
			return -ENOMEM;
		}

		memset(task->aux_iovs[i].iov_base, 0, transfer_size_with_md);
		task->aux_iovs[i].iov_len = transfer_size_with_md;
	}

	return 0;
}

static void
free_dif_generate_copy_sequence_bufs(struct dif_task *task)
{
	uint32_t i = 0;

	if (task->dst_iovs != NULL) {
		for (i = 0; i < task->dst_iovcnt; i++) {
			if (task->dst_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->dst_iovs[i].iov_base);
			}
		}
		free(task->dst_iovs);
	}

	if (task->src_iovs != NULL) {
		for (i = 0; i < task->src_iovcnt; i++) {
			if (task->src_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->src_iovs[i].iov_base);
			}
		}
		free(task->src_iovs);
	}

	if (task->aux_iovs != NULL) {
		for (i = 0; i < task->aux_iovcnt; i++) {
			if (task->aux_iovs[i].iov_base != NULL) {
				spdk_dma_free(task->aux_iovs[i].iov_base);
			}
		}
		free(task->aux_iovs);
	}
}

static void
accel_dif_verify_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	rc = spdk_accel_submit_dif_verify(req->channel, req->src_iovs, req->src_iovcnt,
					  req->num_blocks, req->ctx, req->err,
					  req->cb_fn, req->cb_arg);
	if (rc) {
		wake_ut_thread();
	}
}

static void
accel_dix_verify_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	rc = spdk_accel_submit_dix_verify(req->channel, req->src_iovs, req->src_iovcnt, req->md_iov,
					  req->num_blocks, req->ctx, req->err, req->cb_fn,
					  req->cb_arg);
	if (rc) {
		wake_ut_thread();
	}
}

static void
accel_dix_generate_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	rc = spdk_accel_submit_dix_generate(req->channel, req->src_iovs, req->src_iovcnt,
					    req->md_iov, req->num_blocks, req->ctx, req->cb_fn, req->cb_arg);
	if (rc) {
		wake_ut_thread();
	}
}

static void
accel_dif_verify_copy_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	rc = spdk_accel_submit_dif_verify_copy(req->channel, req->dst_iovs, req->dst_iovcnt,
					       req->src_iovs, req->src_iovcnt,
					       req->num_blocks, req->ctx, req->err,
					       req->cb_fn, req->cb_arg);
	if (rc) {
		wake_ut_thread();
	}
}

static void
accel_dif_generate_copy_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	rc = spdk_accel_submit_dif_generate_copy(req->channel, req->dst_iovs, req->dst_iovcnt,
			req->src_iovs, req->src_iovcnt, req->num_blocks, req->ctx,
			req->cb_fn, req->cb_arg);
	if (rc) {
		wake_ut_thread();
	}
}

static void
accel_dif_generate_copy_sequence_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	req->sequence = NULL;

	rc = spdk_accel_append_dif_generate_copy(&req->sequence, req->channel,
			req->aux_iovs, req->aux_iovcnt, NULL, NULL,
			req->src_iovs, req->src_iovcnt, NULL, NULL,
			req->num_blocks, req->ctx, NULL, NULL);
	if (rc) {
		wake_ut_thread();
		return;
	}

	rc = spdk_accel_append_copy(&req->sequence, req->channel,
				    req->dst_iovs, req->dst_iovcnt, NULL, NULL,
				    req->aux_iovs, req->aux_iovcnt, NULL, NULL,
				    NULL, NULL);
	if (rc) {
		spdk_accel_sequence_abort(req->sequence);
		wake_ut_thread();
		return;
	}

	spdk_accel_sequence_finish(req->sequence, req->cb_fn, req->cb_arg);
}

static void
accel_dif_verify_copy_sequence_test(void *arg)
{
	int rc;
	struct accel_dif_request *req = arg;

	g_completion_success = false;
	req->sequence = NULL;

	rc = spdk_accel_append_dif_verify_copy(&req->sequence, req->channel,
					       req->dst_iovs, req->dst_iovcnt, NULL, NULL,
					       req->dst_iovs, req->dst_iovcnt, NULL, NULL,
					       req->num_blocks, req->ctx, req->err,
					       NULL, NULL);
	if (rc) {
		wake_ut_thread();
		return;
	}

	rc = spdk_accel_append_copy(&req->sequence, req->channel,
				    req->dst_iovs, req->dst_iovcnt, NULL, NULL,
				    req->src_iovs, req->src_iovcnt, NULL, NULL,
				    NULL, NULL);
	if (rc) {
		spdk_accel_sequence_abort(req->sequence);
		wake_ut_thread();
		return;
	}

	spdk_accel_sequence_reverse(req->sequence);
	spdk_accel_sequence_finish(req->sequence, req->cb_fn, req->cb_arg);
}

static void
accel_dif_verify_op_dif_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_bufs(task);
}

static void
accel_dix_generate_verify(struct accel_dif_request *req,
			  uint32_t dif_flags_generate, uint32_t dif_flags_verify)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dix_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       dif_flags_generate,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       dif_flags_verify,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req->channel = g_channel;
	req->src_iovs = task->src_iovs;
	req->src_iovcnt = task->src_iovcnt;
	req->md_iov = &task->md_iov;
	req->num_blocks = task->num_blocks;
	req->ctx = &task->dif_ctx;
	req->err = &task->dif_err;
	req->cb_fn = accel_dif_oper_done;
	req->cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, req);

	free_dix_bufs(task);
}

static void
accel_dif_verify_op_dif_generated_guard_check(void)
{
	accel_dif_verify_op_dif_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_verify_op_dif_generated_apptag_check(void)
{
	accel_dif_verify_op_dif_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_verify_op_dif_generated_reftag_check(void)
{
	accel_dif_verify_op_dif_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dix_verify_op_dix_generated_guard_check(void)
{
	struct accel_dif_request req;
	const char *module_name = NULL;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK, SPDK_DIF_FLAGS_GUARD_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, true);
}

static void
accel_dix_verify_op_dix_generated_apptag_check(void)
{
	struct accel_dif_request req;
	const char *module_name = NULL;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK, SPDK_DIF_FLAGS_APPTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, true);
}

static void
accel_dix_verify_op_dix_generated_reftag_check(void)
{
	struct accel_dif_request req;
	const char *module_name = NULL;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK, SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, true);
}

static void
accel_dix_verify_op_dix_generated_all_flags_check(void)
{
	struct accel_dif_request req;
	accel_dix_generate_verify(&req, SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, true);
}

static void
accel_dix_verify_op_dix_not_generated_all_flags_check(void)
{
	struct accel_dif_request req;
	accel_dix_generate_verify(&req, 0,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, false);
}

static void
accel_dif_verify_op_dif_not_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(dif_flags, req.err), true);

	free_dif_verify_bufs(task);
}

static void
accel_dif_verify_op_dif_not_generated_guard_check(void)
{
	accel_dif_verify_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dix_verify_op_dix_not_generated_guard_check(void)
{
	const char *module_name = NULL;
	uint32_t dif_flags_verify = SPDK_DIF_FLAGS_GUARD_CHECK;
	struct accel_dif_request req;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, 0, dif_flags_verify);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(dif_flags_verify, req.err), true);
}

static void
accel_dif_verify_op_dif_not_generated_apptag_check(void)
{
	accel_dif_verify_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dix_verify_op_dix_not_generated_apptag_check(void)
{
	const char *module_name = NULL;
	uint32_t dif_flags_verify = SPDK_DIF_FLAGS_APPTAG_CHECK;
	struct accel_dif_request req;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, 0, dif_flags_verify);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(dif_flags_verify, req.err), true);
}

static void
accel_dif_verify_op_dif_not_generated_reftag_check(void)
{
	accel_dif_verify_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dix_verify_op_dix_not_generated_reftag_check(void)
{
	const char *module_name = NULL;
	uint32_t dif_flags_verify = SPDK_DIF_FLAGS_REFTAG_CHECK;
	struct accel_dif_request req;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	accel_dix_generate_verify(&req, 0, dif_flags_verify);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(dif_flags_verify, req.err), true);
}

static void
accel_dix_verify_op_dix_guard_not_generated_all_flags_check(void)
{
	struct accel_dif_request req;
	accel_dix_generate_verify(&req,
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(SPDK_DIF_FLAGS_GUARD_CHECK, req.err), true);
}

static void
accel_dix_verify_op_dix_apptag_not_generated_all_flags_check(void)
{
	struct accel_dif_request req;
	accel_dix_generate_verify(&req,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(SPDK_DIF_FLAGS_APPTAG_CHECK, req.err), true);
}

static void
accel_dix_verify_op_dix_reftag_not_generated_all_flags_check(void)
{
	struct accel_dif_request req;
	accel_dix_generate_verify(&req,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK,
				  SPDK_DIF_FLAGS_GUARD_CHECK |
				  SPDK_DIF_FLAGS_APPTAG_CHECK |
				  SPDK_DIF_FLAGS_REFTAG_CHECK);

	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(SPDK_DIF_FLAGS_REFTAG_CHECK, req.err), true);
}

static void
accel_dif_verify_op_apptag_correct_apptag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_bufs(task);
}

static void
accel_dix_verify_op_apptag_correct_apptag_check(void)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	rc = alloc_dix_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, &req);

	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dix_bufs(task);
}

static void
accel_dif_verify_op_apptag_incorrect_apptag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       30, 0xFFFF, 40, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);

	free_dif_verify_bufs(task);
}

static void
accel_dix_verify_op_apptag_incorrect_apptag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dix_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       30, 0xFFFF, 40, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);

	free_dix_bufs(task);
}

static void
accel_dif_verify_op_tag_incorrect_no_check_or_ignore(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	/* For set 'Application Tag F Detect' (Source DIF Flags)
	 * When all bits of the Application Tag field of the source Data Integrity Field
	 * are equal to 1, the Application Tag check is not done and the Guard field and
	 * Reference Tag field are ignored. */
	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 0xFFFF, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       30, 0xFFFF, 40, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_bufs(task);
}

static void
accel_dix_verify_op_tag_incorrect_no_check_or_ignore(uint32_t dif_flags)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verify for DIX */
	if (!strcmp(module_name, "dsa") && (dif_flags != (SPDK_DIF_FLAGS_GUARD_CHECK |
					    SPDK_DIF_FLAGS_APPTAG_CHECK |
					    SPDK_DIF_FLAGS_REFTAG_CHECK))) {
		return;
	}

	rc = alloc_dix_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	/* For set 'Application Tag F Detect' (Source DIF Flags)
	 * When all bits of the Application Tag field of the source Data Integrity Field
	 * are equal to 1, the Application Tag check is not done and the Guard field and
	 * Reference Tag field are ignored. */
	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 0xFFFF, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       30, 0xFFFF, 40, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, &req);

	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dix_bufs(task);
}

static void
accel_dif_verify_op_apptag_incorrect_no_apptag_check(void)
{
	accel_dif_verify_op_tag_incorrect_no_check_or_ignore(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dix_verify_op_apptag_incorrect_no_apptag_check(void)
{
	accel_dix_verify_op_tag_incorrect_no_check_or_ignore(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_verify_op_reftag_incorrect_reftag_ignore(void)
{
	accel_dif_verify_op_tag_incorrect_no_check_or_ignore(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dix_verify_op_reftag_incorrect_reftag_ignore(void)
{
	accel_dix_verify_op_tag_incorrect_no_check_or_ignore(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dif_verify_op_reftag_init_correct_reftag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 2);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_bufs(task);
}

static void
accel_dix_verify_op_reftag_init_correct_reftag_check(void)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIX_VERIFY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Intel DSA does not allow for selective DIF fields verification for DIX */
	if (!strcmp(module_name, "dsa")) {
		return;
	}

	rc = alloc_dix_bufs(task, 2);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, &req);

	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dix_bufs(task);
}

static void
accel_dif_verify_op_reftag_init_incorrect_reftag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_bufs(task, 2);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);

	free_dif_verify_bufs(task);
}

static void
accel_dix_verify_op_reftag_init_incorrect_reftag_check(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dix_bufs(task, 2);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_generate(task->src_iovs, task->src_iovcnt, &task->md_iov, task->num_blocks,
			       &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_verify_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);

	free_dix_bufs(task);
}

static void
accel_dif_verify_copy_op_dif_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_copy_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_copy_bufs(task);
}

static void
accel_dif_verify_copy_op_dif_generated_guard_check(void)
{
	accel_dif_verify_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_verify_copy_op_dif_generated_apptag_check(void)
{
	accel_dif_verify_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_verify_copy_op_dif_generated_reftag_check(void)
{
	accel_dif_verify_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dif_verify_copy_op_dif_not_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_copy_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);
	CU_ASSERT_EQUAL(accel_dif_error_validate(dif_flags, req.err), true);

	free_dif_verify_copy_bufs(task);
}

static void
accel_dif_verify_copy_op_dif_not_generated_guard_check(void)
{
	accel_dif_verify_copy_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_verify_copy_op_dif_not_generated_apptag_check(void)
{
	accel_dif_verify_copy_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_verify_copy_op_dif_not_generated_reftag_check(void)
{
	accel_dif_verify_copy_op_dif_not_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dif_generate_copy_op_dif_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	struct spdk_dif_error err_blk;
	int rc;

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_verify(req.dst_iovs, req.dst_iovcnt, req.num_blocks,
			     &task->dif_ctx, &err_blk);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	free_dif_generate_copy_bufs(task);
}

static void
accel_dix_generate_op_dix_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	struct spdk_dif_error err_blk;
	int rc;

	rc = alloc_dix_bufs(task, 3);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.md_iov = &task->md_iov;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dix_generate_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes,
			       g_md_size_bytes, false, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dix_verify(req.src_iovs, req.src_iovcnt, req.md_iov, req.num_blocks,
			     &task->dif_ctx, &err_blk);
	CU_ASSERT_EQUAL(rc, 0);

	free_dix_bufs(task);
}

static void
accel_dif_generate_copy_op_dif_generated_guard_check(void)
{
	accel_dif_generate_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dix_generate_op_dix_generated_guard_check(void)
{
	accel_dix_generate_op_dix_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_generate_copy_op_dif_generated_apptag_check(void)
{
	accel_dif_generate_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dix_generate_op_dix_generated_apptag_check(void)
{
	accel_dix_generate_op_dix_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_generate_copy_op_dif_generated_reftag_check(void)
{
	accel_dif_generate_copy_op_dif_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dix_generate_op_dix_generated_reftag_check(void)
{
	accel_dix_generate_op_dix_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dif_generate_copy_op_dif_generated_no_guard_check_flag_set(void)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIF_GENERATE_COPY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);

	/* Intel DSA does not allow for selective DIF fields generation */
	if (!strcmp(module_name, "dsa")) {
		CU_ASSERT_EQUAL(g_completion_success, false);
	} else if (!strcmp(module_name, "software")) {
		CU_ASSERT_EQUAL(g_completion_success, true);
	} else {
		SPDK_CU_ASSERT_FATAL(false);
	}

	free_dif_generate_copy_bufs(task);
}

static void
accel_dif_generate_copy_op_dif_generated_no_apptag_check_flag_set(void)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIF_GENERATE_COPY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);

	/* Intel DSA does not allow for selective DIF fields generation */
	if (!strcmp(module_name, "dsa")) {
		CU_ASSERT_EQUAL(g_completion_success, false);
	} else if (!strcmp(module_name, "software")) {
		CU_ASSERT_EQUAL(g_completion_success, true);
	} else {
		SPDK_CU_ASSERT_FATAL(false);
	}

	free_dif_generate_copy_bufs(task);
}

static void
accel_dif_generate_copy_op_dif_generated_no_reftag_check_flag_set(void)
{
	const char *module_name = NULL;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = spdk_accel_get_opc_module_name(SPDK_ACCEL_OPC_DIF_GENERATE_COPY, &module_name);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);

	/* Intel DSA does not allow for selective DIF fields generation */
	if (!strcmp(module_name, "dsa")) {
		CU_ASSERT_EQUAL(g_completion_success, false);
	} else if (!strcmp(module_name, "software")) {
		CU_ASSERT_EQUAL(g_completion_success, true);
	} else {
		SPDK_CU_ASSERT_FATAL(false);
	}

	free_dif_generate_copy_bufs(task);
}

static void
accel_dif_generate_copy_op_iovecs_len_validate(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	/* Make iov_len param incorrect */
	req.dst_iovs->iov_len -= 16;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, false);

	free_dif_generate_copy_bufs(task);
}

static void
accel_dif_generate_copy_op_buf_align_validate(void)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_generate_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_generate_copy_bufs(task);
}

static void
accel_dif_generate_copy_sequence_dif_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	struct spdk_dif_error err_blk;
	int rc;

	rc = alloc_dif_generate_copy_sequence_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.aux_iovs = task->aux_iovs;
	req.aux_iovcnt = task->aux_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_generate_copy_sequence_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       16, 0xFFFF, 10, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_verify(req.dst_iovs, req.dst_iovcnt, req.num_blocks,
			     &task->dif_ctx, &err_blk);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	free_dif_generate_copy_sequence_bufs(task);
}

static void
accel_dif_generate_copy_sequence_dif_generated_guard_check(void)
{
	accel_dif_generate_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_generate_copy_sequence_dif_generated_apptag_check(void)
{
	accel_dif_generate_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_generate_copy_sequence_dif_generated_reftag_check(void)
{
	accel_dif_generate_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
accel_dif_verify_copy_sequence_dif_generated_do_check(uint32_t dif_flags)
{
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	struct accel_dif_request req;
	struct dif_task *task = &g_dif_task;
	int rc;

	rc = alloc_dif_verify_copy_bufs(task, 1);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK |
			       SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_generate(task->src_iovs, task->src_iovcnt, task->num_blocks, &task->dif_ctx);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	rc = spdk_dif_ctx_init(&task->dif_ctx,
			       g_block_size_bytes + g_md_size_bytes,
			       g_md_size_bytes, true, true,
			       SPDK_DIF_TYPE1,
			       dif_flags,
			       10, 0xFFFF, 20, 0, 0, &dif_opts);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	req.channel = g_channel;
	req.dst_iovs = task->dst_iovs;
	req.dst_iovcnt = task->dst_iovcnt;
	req.src_iovs = task->src_iovs;
	req.src_iovcnt = task->src_iovcnt;
	req.num_blocks = task->num_blocks;
	req.ctx = &task->dif_ctx;
	req.err = &task->dif_err;
	req.cb_fn = accel_dif_oper_done;
	req.cb_arg = task;

	execute_spdk_function(accel_dif_verify_copy_sequence_test, &req);
	CU_ASSERT_EQUAL(g_completion_success, true);

	free_dif_verify_copy_bufs(task);
}

static void
accel_dif_verify_copy_sequence_dif_generated_guard_check(void)
{
	accel_dif_verify_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_GUARD_CHECK);
}

static void
accel_dif_verify_copy_sequence_dif_generated_apptag_check(void)
{
	accel_dif_verify_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_APPTAG_CHECK);
}

static void
accel_dif_verify_copy_sequence_dif_generated_reftag_check(void)
{
	accel_dif_verify_copy_sequence_dif_generated_do_check(SPDK_DIF_FLAGS_REFTAG_CHECK);
}

static void
_stop_init_thread(void *arg)
{
	unsigned num_failures = g_num_failures;

	g_num_failures = 0;

	assert(spdk_get_thread() == g_thread[WORKER_UT]);
	assert(spdk_thread_is_app_thread(NULL));
	execute_spdk_function(exit_io_thread, NULL);
	spdk_app_stop(num_failures);
}

static void
stop_init_thread(unsigned num_failures, struct spdk_jsonrpc_request *request)
{
	g_num_failures = num_failures;

	spdk_thread_send_msg(g_thread[WORKER_UT], _stop_init_thread, request);
}

static int
setup_accel_tests(void)
{
	unsigned rc = 0;
	CU_pSuite suite = NULL;

	suite = CU_add_suite("accel_dif", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		rc = CU_get_error();
		return -rc;
	}

	if (CU_add_test(suite, "verify: DIF generated, GUARD check",
			accel_dif_verify_op_dif_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify: DIX generated, GUARD check",
			accel_dix_verify_op_dix_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify: DIF generated, APPTAG check",
			accel_dif_verify_op_dif_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX generated, APPTAG check",
			accel_dix_verify_op_dix_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF generated, REFTAG check",
			accel_dif_verify_op_dif_generated_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX generated, REFTAG check",
			accel_dix_verify_op_dix_generated_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX generated, all flags check",
			accel_dix_verify_op_dix_generated_all_flags_check) == NULL ||

	    CU_add_test(suite, "verify: DIF not generated, GUARD check",
			accel_dif_verify_op_dif_not_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify: DIX not generated, GUARD check",
			accel_dix_verify_op_dix_not_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify: DIF not generated, APPTAG check",
			accel_dif_verify_op_dif_not_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX not generated, APPTAG check",
			accel_dix_verify_op_dix_not_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF not generated, REFTAG check",
			accel_dif_verify_op_dif_not_generated_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX not generated, REFTAG check",
			accel_dix_verify_op_dix_not_generated_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX not generated, all flags check",
			accel_dix_verify_op_dix_not_generated_all_flags_check) == NULL ||
	    CU_add_test(suite, "verify: DIX guard not generated, all flags check",
			accel_dix_verify_op_dix_guard_not_generated_all_flags_check) == NULL ||
	    CU_add_test(suite, "verify: DIX apptag not generated, all flags check",
			accel_dix_verify_op_dix_apptag_not_generated_all_flags_check) == NULL ||
	    CU_add_test(suite, "verify: DIX reftag not generated, all flags check",
			accel_dix_verify_op_dix_reftag_not_generated_all_flags_check) == NULL ||

	    CU_add_test(suite, "verify: DIF APPTAG correct, APPTAG check",
			accel_dif_verify_op_apptag_correct_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX APPTAG correct, APPTAG check",
			accel_dix_verify_op_apptag_correct_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF APPTAG incorrect, APPTAG check",
			accel_dif_verify_op_apptag_incorrect_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX APPTAG incorrect, APPTAG check",
			accel_dix_verify_op_apptag_incorrect_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF APPTAG incorrect, no APPTAG check",
			accel_dif_verify_op_apptag_incorrect_no_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX APPTAG incorrect, no APPTAG check",
			accel_dix_verify_op_apptag_incorrect_no_apptag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF REFTAG incorrect, REFTAG ignore",
			accel_dif_verify_op_reftag_incorrect_reftag_ignore) == NULL ||
	    CU_add_test(suite, "verify: DIX REFTAG incorrect, REFTAG ignore",
			accel_dix_verify_op_reftag_incorrect_reftag_ignore) == NULL ||

	    CU_add_test(suite, "verify: DIF REFTAG_INIT correct, REFTAG check",
			accel_dif_verify_op_reftag_init_correct_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX REFTAG_INIT correct, REFTAG check",
			accel_dix_verify_op_reftag_init_correct_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIF REFTAG_INIT incorrect, REFTAG check",
			accel_dif_verify_op_reftag_init_incorrect_reftag_check) == NULL ||
	    CU_add_test(suite, "verify: DIX REFTAG_INIT incorrect, REFTAG check",
			accel_dix_verify_op_reftag_init_incorrect_reftag_check) == NULL ||

	    CU_add_test(suite, "verify copy: DIF generated, GUARD check",
			accel_dif_verify_copy_op_dif_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify copy: DIF generated, APPTAG check",
			accel_dif_verify_copy_op_dif_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify copy: DIF generated, REFTAG check",
			accel_dif_verify_copy_op_dif_generated_reftag_check) == NULL ||

	    CU_add_test(suite, "verify copy: DIF not generated, GUARD check",
			accel_dif_verify_copy_op_dif_not_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify copy: DIF not generated, APPTAG check",
			accel_dif_verify_copy_op_dif_not_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify copy: DIF not generated, REFTAG check",
			accel_dif_verify_copy_op_dif_not_generated_reftag_check) == NULL ||

	    CU_add_test(suite, "generate copy: DIF generated, GUARD check",
			accel_dif_generate_copy_op_dif_generated_guard_check) == NULL ||
	    CU_add_test(suite, "generate copy: DIF generated, APTTAG check",
			accel_dif_generate_copy_op_dif_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "generate copy: DIF generated, REFTAG check",
			accel_dif_generate_copy_op_dif_generated_reftag_check) == NULL ||

	    CU_add_test(suite, "generate: DIX generated, GUARD check",
			accel_dix_generate_op_dix_generated_guard_check) == NULL ||
	    CU_add_test(suite, "generate: DIX generated, APTTAG check",
			accel_dix_generate_op_dix_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "generate: DIX generated, REFTAG check",
			accel_dix_generate_op_dix_generated_reftag_check) == NULL ||

	    CU_add_test(suite, "generate copy: DIF generated, no GUARD check flag set",
			accel_dif_generate_copy_op_dif_generated_no_guard_check_flag_set) == NULL ||
	    CU_add_test(suite, "generate copy: DIF generated, no APPTAG check flag set",
			accel_dif_generate_copy_op_dif_generated_no_apptag_check_flag_set) == NULL ||
	    CU_add_test(suite, "generate copy: DIF generated, no REFTAG check flag set",
			accel_dif_generate_copy_op_dif_generated_no_reftag_check_flag_set) == NULL ||

	    CU_add_test(suite, "generate copy: DIF iovecs-len validate",
			accel_dif_generate_copy_op_iovecs_len_validate) == NULL ||
	    CU_add_test(suite, "generate copy: DIF buffer alignment validate",
			accel_dif_generate_copy_op_buf_align_validate) == NULL ||

	    CU_add_test(suite, "generate copy sequence: DIF generated, GUARD check",
			accel_dif_generate_copy_sequence_dif_generated_guard_check) == NULL ||
	    CU_add_test(suite, "generate copy sequence: DIF generated, APTTAG check",
			accel_dif_generate_copy_sequence_dif_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "generate copy sequence: DIF generated, REFTAG check",
			accel_dif_generate_copy_sequence_dif_generated_reftag_check) == NULL ||
	    CU_add_test(suite, "verify copy sequence: DIF generated, GUARD check",
			accel_dif_verify_copy_sequence_dif_generated_guard_check) == NULL ||
	    CU_add_test(suite, "verify copy sequence: DIF generated, APPTAG check",
			accel_dif_verify_copy_sequence_dif_generated_apptag_check) == NULL ||
	    CU_add_test(suite, "verify copy sequence: DIF generated, REFTAG check",
			accel_dif_verify_copy_sequence_dif_generated_reftag_check) == NULL) {
		CU_cleanup_registry();
		rc = CU_get_error();
		return -rc;
	}
	return 0;
}

static void
get_io_channel(void *arg)
{
	g_channel = spdk_accel_get_io_channel();
	assert(g_channel);
	wake_ut_thread();
}

static void
put_io_channel(void *arg)
{
	assert(g_channel);
	spdk_put_io_channel(g_channel);
	wake_ut_thread();
}

static void
run_accel_test_thread(void *arg)
{
	struct spdk_jsonrpc_request *request = arg;
	int rc = 0;

	execute_spdk_function(get_io_channel, NULL);
	if (g_channel == NULL) {
		fprintf(stderr, "Unable to get an accel channel\n");
		goto ret;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		/* CUnit error, probably won't recover */
		rc = CU_get_error();
		rc = -rc;
		goto ret;
	}

	rc = setup_accel_tests();
	if (rc < 0) {
		/* CUnit error, probably won't recover */
		rc = -rc;
		goto ret;
	}
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	rc = CU_get_number_of_failures();
	CU_cleanup_registry();

ret:
	if (g_channel != NULL) {
		execute_spdk_function(put_io_channel, NULL);
	}
	stop_init_thread(rc, request);
}

static void
accel_dif_test_main(void *arg1)
{
	struct spdk_cpuset tmpmask = {};
	uint32_t i;

	pthread_mutex_init(&g_test_mutex, NULL);
	pthread_cond_init(&g_test_cond, NULL);

	/* This test runs specifically on at least two cores.
	 * g_thread[WORKER_UT] is the app_thread on main core from event framework.
	 * Next one is only for the tests and should always be on separate CPU cores. */
	if (spdk_env_get_core_count() < 3) {
		spdk_app_stop(-1);
		return;
	}

	SPDK_ENV_FOREACH_CORE(i) {
		if (i == spdk_env_get_current_core()) {
			g_thread[WORKER_UT] = spdk_get_thread();
			continue;
		}
		spdk_cpuset_zero(&tmpmask);
		spdk_cpuset_set_cpu(&tmpmask, i, true);
		if (g_thread[WORKER_IO] == NULL) {
			g_thread[WORKER_IO] = spdk_thread_create("io_thread", &tmpmask);
		}

	}

	spdk_thread_send_msg(g_thread[WORKER_UT], run_accel_test_thread, NULL);
}

static void
accel_dif_usage(void)
{
}

static int
accel_dif_parse_arg(int ch, char *arg)
{
	return 0;
}

static void
spdk_dif_shutdown_cb(void)
{
	g_shutdown = true;
	spdk_thread_send_msg(g_thread[WORKER_UT], _stop_init_thread, NULL);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	char reactor_mask[8];
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "DIF";
	snprintf(reactor_mask, sizeof(reactor_mask), "0x%x", (1 << (SPDK_COUNTOF(g_thread) + 1)) - 1);
	opts.reactor_mask = reactor_mask;
	opts.shutdown_cb = spdk_dif_shutdown_cb;
	opts.rpc_addr = NULL;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				      accel_dif_parse_arg, accel_dif_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, accel_dif_test_main, NULL);
	spdk_app_fini();

	return rc;
}
