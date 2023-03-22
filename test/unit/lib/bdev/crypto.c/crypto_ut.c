/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "unit/lib/json_mock.c"

#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include <rte_version.h>

unsigned ut_rte_crypto_op_bulk_alloc;
int ut_rte_crypto_op_attach_sym_session = 0;
#define MOCK_INFO_GET_1QP_AESNI 0
#define MOCK_INFO_GET_1QP_QAT 1
#define MOCK_INFO_GET_1QP_MLX5 2
#define MOCK_INFO_GET_1QP_BOGUS_PMD 3
int ut_rte_cryptodev_info_get = 0;
bool ut_rte_cryptodev_info_get_mocked = false;

#include "bdev/crypto/vbdev_crypto.c"

/* SPDK stubs */
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB_V(spdk_bdev_io_put_aux_buf, (struct spdk_bdev_io *bdev_io, void *aux_buf));
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_buf_align, size_t, (const struct spdk_bdev *bdev), 64);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));
DEFINE_STUB(spdk_bdev_unregister_by_name, int, (const char *bdev_name,
		struct spdk_bdev_module *module,
		spdk_bdev_unregister_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb,
				      void *event_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *vbdev), 0);
DEFINE_STUB_V(spdk_bdev_destruct_done, (struct spdk_bdev *bdev, int bdeverrno));

DEFINE_STUB(spdk_accel_crypto_key_destroy, int, (struct spdk_accel_crypto_key *key), 0);

/* global vars and setup/cleanup functions used for all test functions */
struct spdk_bdev_io *g_bdev_io;
struct crypto_bdev_io *g_io_ctx;
struct crypto_io_channel *g_crypto_ch;
struct spdk_io_channel *g_io_ch;
struct vbdev_crypto g_crypto_bdev;
struct vbdev_crypto_opts g_crypto_bdev_opts;

void
spdk_bdev_io_get_aux_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_aux_buf_cb cb)
{
	cb(g_io_ch, g_bdev_io, (void *)0xDEADBEEF);
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(g_io_ch, g_bdev_io, true);
}

struct ut_vbdev_crypto_bdev_cpl_args {
	spdk_bdev_io_completion_cb cb_fn;
	struct spdk_bdev_io *bdev_io;
	void *cb_arg;
	bool result;
};

static void
_ut_vbdev_crypto_bdev_cpl(void *arg)
{
	struct ut_vbdev_crypto_bdev_cpl_args *cpl_args = arg;

	cpl_args->cb_fn(cpl_args->bdev_io, cpl_args->result, cpl_args->cb_arg);
	free(cpl_args);
}

static void
ut_vbdev_crypto_bdev_cpl(spdk_bdev_io_completion_cb cb_fn, struct spdk_bdev_io *bdev_io,
			 bool result, void *cb_arg)
{
	struct ut_vbdev_crypto_bdev_cpl_args *cpl_args = calloc(1, sizeof(*cpl_args));

	SPDK_CU_ASSERT_FATAL(cpl_args);
	cpl_args->cb_fn = cb_fn;
	cpl_args->bdev_io = bdev_io;
	cpl_args->result = result;
	cpl_args->cb_arg = cb_arg;

	spdk_thread_send_msg(spdk_get_thread(), _ut_vbdev_crypto_bdev_cpl, cpl_args);
}

/* Mock these functions to call the callback and then return the value we require */
DEFINE_RETURN_MOCK(spdk_bdev_readv_blocks, int);
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_bdev_readv_blocks);
	ut_vbdev_crypto_bdev_cpl(cb, g_bdev_io, !ut_spdk_bdev_readv_blocks, cb_arg);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_bdev_writev_blocks, int);
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_bdev_writev_blocks);
	ut_vbdev_crypto_bdev_cpl(cb, g_bdev_io, !ut_spdk_bdev_writev_blocks, cb_arg);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_bdev_unmap_blocks, int);
int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_bdev_unmap_blocks);
	ut_vbdev_crypto_bdev_cpl(cb, g_bdev_io, !ut_spdk_bdev_unmap_blocks, cb_arg);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_bdev_flush_blocks, int);
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		       void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_bdev_flush_blocks);
	ut_vbdev_crypto_bdev_cpl(cb, g_bdev_io, !ut_spdk_bdev_flush_blocks, cb_arg);
	return 0;
}

DEFINE_RETURN_MOCK(spdk_bdev_reset, int);
int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_bdev_reset);
	ut_vbdev_crypto_bdev_cpl(cb, g_bdev_io, !ut_spdk_bdev_reset, cb_arg);
	return 0;
}

bool g_completion_called = false;
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
	g_completion_called = true;
}

struct ut_vbdev_crypto_accel_cpl_args {
	spdk_accel_completion_cb cb_fn;
	void *cb_arg;
	int rc;
};

static void
_vbdev_crypto_ut_accel_cpl(void *arg)
{
	struct ut_vbdev_crypto_accel_cpl_args *cpl_args = arg;

	cpl_args->cb_fn(cpl_args->cb_arg, cpl_args->rc);
	free(cpl_args);
}

static void
vbdev_crypto_ut_accel_cpl(spdk_accel_completion_cb cb_fn, void *cb_arg, int rc)
{
	struct ut_vbdev_crypto_accel_cpl_args *cpl_args = calloc(1, sizeof(*cpl_args));

	SPDK_CU_ASSERT_FATAL(cpl_args);
	cpl_args->cb_fn = cb_fn;
	cpl_args->cb_arg = cb_arg;
	cpl_args->rc = rc;

	spdk_thread_send_msg(spdk_get_thread(), _vbdev_crypto_ut_accel_cpl, cpl_args);
}

DEFINE_RETURN_MOCK(spdk_accel_submit_encrypt, int);
int ut_spdk_accel_submit_encrypt_cb_rc = 0;
int
spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_accel_submit_encrypt);
	/* We must not call cb_fn immediately */
	vbdev_crypto_ut_accel_cpl(cb_fn, cb_arg, ut_spdk_accel_submit_encrypt_cb_rc);

	return 0;
}

int ut_spdk_accel_submit_decrypt_cb_rc;
DEFINE_RETURN_MOCK(spdk_accel_submit_decrypt, int);
int
spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	HANDLE_RETURN_MOCK(spdk_accel_submit_decrypt);
	/* We must not call cb_fn immediately */
	vbdev_crypto_ut_accel_cpl(cb_fn, cb_arg, ut_spdk_accel_submit_decrypt_cb_rc);

	return 0;
}

struct spdk_io_channel *spdk_accel_get_io_channel(void)
{
	return (struct spdk_io_channel *)0xfeedbeef;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	/* Prepare essential variables for test routines */
	g_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct crypto_bdev_io));
	g_bdev_io->u.bdev.iovs = calloc(1, sizeof(struct iovec) * 128);
	g_bdev_io->bdev = &g_crypto_bdev.crypto_bdev;
	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct crypto_io_channel));
	g_crypto_ch = (struct crypto_io_channel *)spdk_io_channel_get_ctx(g_io_ch);
	g_io_ctx = (struct crypto_bdev_io *)g_bdev_io->driver_ctx;
	memset(&g_crypto_bdev, 0, sizeof(struct vbdev_crypto));
	memset(&g_crypto_bdev_opts, 0, sizeof(struct vbdev_crypto_opts));
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_io_ctx->crypto_ch = g_crypto_ch;
	g_io_ctx->crypto_bdev = &g_crypto_bdev;
	g_io_ctx->crypto_bdev->opts = &g_crypto_bdev_opts;
	TAILQ_INIT(&g_crypto_ch->in_accel_fw);

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	free(g_bdev_io->u.bdev.iovs);
	free(g_bdev_io);
	free(g_io_ch);
	return 0;
}

static void
test_error_paths(void)
{
	g_crypto_bdev.crypto_bdev.blocklen = 512;

	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = (void *)0xDEADBEEF;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;

	/* test error returned by accel fw */
	MOCK_SET(spdk_accel_submit_encrypt, -ENOMEM);
	g_completion_called = false;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM);
	CU_ASSERT(g_completion_called);

	MOCK_SET(spdk_accel_submit_encrypt, -EINVAL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_accel_submit_encrypt);

	/* Test error returned in accel cpl cb */
	ut_spdk_accel_submit_encrypt_cb_rc = -EINVAL;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_spdk_accel_submit_encrypt_cb_rc = 0;

	/* Test error returned from bdev */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_bdev_writev_blocks, -ENOMEM);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->bdev_io_wait.bdev == &g_crypto_bdev.crypto_bdev);
	CU_ASSERT(g_io_ctx->bdev_io_wait.cb_fn == vbdev_crypto_resubmit_io);
	CU_ASSERT(g_io_ctx->bdev_io_wait.cb_arg == g_bdev_io);
	CU_ASSERT(g_io_ctx->resubmit_state == CRYPTO_IO_ENCRYPT_DONE);
	memset(&g_io_ctx->bdev_io_wait, 0, sizeof(g_io_ctx->bdev_io_wait));
	MOCK_CLEAR(spdk_bdev_readv_blocks);

	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_bdev_writev_blocks, -EINVAL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_writev_blocks);

	/* Test error returned in bdev cpl */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	ut_spdk_bdev_writev_blocks = -EINVAL;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_spdk_bdev_writev_blocks = 0;

	/* the same for read path */
	/* Test error returned from bdev */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;

	MOCK_SET(spdk_bdev_readv_blocks, -ENOMEM);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->bdev_io_wait.bdev == &g_crypto_bdev.crypto_bdev);
	CU_ASSERT(g_io_ctx->bdev_io_wait.cb_fn == vbdev_crypto_resubmit_io);
	CU_ASSERT(g_io_ctx->bdev_io_wait.cb_arg == g_bdev_io);
	CU_ASSERT(g_io_ctx->resubmit_state == CRYPTO_IO_NEW);
	memset(&g_io_ctx->bdev_io_wait, 0, sizeof(g_io_ctx->bdev_io_wait));
	MOCK_CLEAR(spdk_bdev_readv_blocks);

	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_bdev_readv_blocks, -EINVAL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_readv_blocks);

	/* Test error returned in bdev cpl */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	ut_spdk_bdev_readv_blocks = -EINVAL;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_spdk_bdev_readv_blocks = 0;

	/* test error returned by accel fw */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	MOCK_SET(spdk_accel_submit_decrypt, -ENOMEM);
	g_completion_called = false;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_NOMEM);
	CU_ASSERT(g_completion_called);
	MOCK_CLEAR(spdk_accel_submit_decrypt);
	g_completion_called = false;

	/* test error returned in accel cpl */
	ut_spdk_accel_submit_decrypt_cb_rc = -EINVAL;
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	ut_spdk_accel_submit_decrypt_cb_rc = 0;

}

static void
test_simple_write(void)
{
	/* Single element block size write */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.offset_blocks = 0;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_simple_write;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	/* 1st poll to trigger accel completions, 2nd for bdev */
	poll_threads();
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_io_ctx->aux_buf_iov.iov_len == 512);
	CU_ASSERT(g_io_ctx->aux_buf_iov.iov_base != NULL);
	CU_ASSERT(g_io_ctx->aux_offset_blocks == 0);
	CU_ASSERT(g_io_ctx->aux_num_blocks == 1);
}

static void
test_simple_read(void)
{
	/* Single element block size read */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->u.bdev.iovcnt = 1;
	g_bdev_io->u.bdev.num_blocks = 1;
	g_bdev_io->u.bdev.iovs[0].iov_len = 512;
	g_bdev_io->u.bdev.iovs[0].iov_base = &test_simple_read;
	g_crypto_bdev.crypto_bdev.blocklen = 512;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;

	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	/* 1st poll to trigger dev completions, 2nd for accel */
	poll_threads();
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
test_passthru(void)
{
	/* Make sure these follow our completion callback, test success & fail. */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_UNMAP;
	MOCK_CLEAR(spdk_bdev_unmap_blocks);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	MOCK_SET(spdk_bdev_unmap_blocks, -EINVAL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_unmap_blocks);

	g_bdev_io->type = SPDK_BDEV_IO_TYPE_FLUSH;
	MOCK_CLEAR(spdk_bdev_flush_blocks);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	MOCK_SET(spdk_bdev_flush_blocks, -EINVAL);
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	MOCK_CLEAR(spdk_bdev_flush_blocks);

	/* We should never get a WZ command, we report that we don't support it. */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	vbdev_crypto_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
}

static void
test_reset(void)
{
	/* TODO: There are a few different ways to do this given that
	 * the code uses spdk_for_each_channel() to implement reset
	 * handling. Submitting w/o UT for this function for now and
	 * will follow up with something shortly.
	 */
}

static void
test_crypto_op_complete(void)
{
	/* Make sure completion code respects failure. */
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io, -1);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);

	/* Test read completion. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io, 0);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* Test write completion success. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	MOCK_CLEAR(spdk_bdev_writev_blocks);
	_crypto_operation_complete(g_bdev_io, 0);
	poll_threads();
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* Test write completion failed. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	MOCK_SET(spdk_bdev_writev_blocks, -EINVAL);
	_crypto_operation_complete(g_bdev_io, 0);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);
	MOCK_CLEAR(spdk_bdev_writev_blocks);

	/* Test bogus type for this completion. */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_RESET;
	g_completion_called = false;
	_crypto_operation_complete(g_bdev_io, 0);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);
}

static void
test_supported_io(void)
{
	void *ctx = NULL;
	bool rc = true;

	/* Make sure we always report false to WZ, we need the bdev layer to
	 * send real 0's so we can encrypt/decrypt them.
	 */
	rc = vbdev_crypto_io_type_supported(ctx, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(rc == false);
}
int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("crypto", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_error_paths);
	CU_ADD_TEST(suite, test_simple_write);
	CU_ADD_TEST(suite, test_simple_read);
	CU_ADD_TEST(suite, test_passthru);
	CU_ADD_TEST(suite, test_crypto_op_complete);
	CU_ADD_TEST(suite, test_supported_io);
	CU_ADD_TEST(suite, test_reset);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();

	free_threads();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
