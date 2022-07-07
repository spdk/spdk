/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
/* We have our own mock for this */
#define UNIT_TEST_NO_VTOPHYS
#include "common/lib/test_env.c"
#include "spdk_internal/mock.h"
#include "thread/thread_internal.h"
#include "unit/lib/json_mock.c"
#include "spdk/reduce.h"


/* There will be one if the data perfectly matches the chunk size,
 * or there could be an offset into the data and a remainder after
 * the data or both for a max of 3.
 */
#define UT_MBUFS_PER_OP 3
/* For testing the crossing of a huge page boundary on address translation,
 * we'll have an extra one but we only test on the source side.
 */
#define UT_MBUFS_PER_OP_BOUND_TEST 4

struct spdk_bdev_io *g_bdev_io;
struct spdk_io_channel *g_io_ch;
struct vbdev_compress g_comp_bdev;
struct comp_bdev_io *g_io_ctx;
struct comp_io_channel *g_comp_ch;

static int ut_spdk_reduce_vol_op_complete_err = 0;
void
spdk_reduce_vol_writev(struct spdk_reduce_vol *vol, struct iovec *iov, int iovcnt,
		       uint64_t offset, uint64_t length, spdk_reduce_vol_op_complete cb_fn,
		       void *cb_arg)
{
	cb_fn(cb_arg, ut_spdk_reduce_vol_op_complete_err);
}

void
spdk_reduce_vol_readv(struct spdk_reduce_vol *vol, struct iovec *iov, int iovcnt,
		      uint64_t offset, uint64_t length, spdk_reduce_vol_op_complete cb_fn,
		      void *cb_arg)
{
	cb_fn(cb_arg, ut_spdk_reduce_vol_op_complete_err);
}

#include "bdev/compress/vbdev_compress.c"

/* SPDK stubs */
DEFINE_STUB(spdk_accel_get_opc_module_name, int, (enum accel_opcode opcode,
		const char **module_name), 0);
DEFINE_STUB(spdk_accel_get_io_channel, struct spdk_io_channel *, (void), (void *)0xfeedbeef);
DEFINE_STUB(spdk_bdev_get_aliases, const struct spdk_bdev_aliases_list *,
	    (const struct spdk_bdev *bdev), NULL);
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), 0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc), 0);
DEFINE_STUB_V(spdk_bdev_unregister, (struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
				     void *cb_arg));
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb,
				      void *event_ctx, struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB_V(spdk_bdev_module_examine_done, (struct spdk_bdev_module *module));
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_bdev_io_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_io *bdev_io),
	    0);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB_V(spdk_reduce_vol_unload, (struct spdk_reduce_vol *vol,
				       spdk_reduce_vol_op_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_load, (struct spdk_reduce_backing_dev *backing_dev,
				     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg));
DEFINE_STUB(spdk_reduce_vol_get_params, const struct spdk_reduce_vol_params *,
	    (struct spdk_reduce_vol *vol), NULL);
DEFINE_STUB_V(spdk_reduce_vol_init, (struct spdk_reduce_vol_params *params,
				     struct spdk_reduce_backing_dev *backing_dev,
				     const char *pm_file_dir,
				     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(spdk_reduce_vol_destroy, (struct spdk_reduce_backing_dev *backing_dev,
					spdk_reduce_vol_op_complete cb_fn, void *cb_arg));

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

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(g_io_ch, g_bdev_io, true);
}

/* Mock these functions to call the callback and then return the value we require */
int ut_spdk_bdev_readv_blocks = 0;
int
spdk_bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iov, int iovcnt,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_readv_blocks, cb_arg);
	return ut_spdk_bdev_readv_blocks;
}

int ut_spdk_bdev_writev_blocks = 0;
bool ut_spdk_bdev_writev_blocks_mocked = false;
int
spdk_bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_writev_blocks, cb_arg);
	return ut_spdk_bdev_writev_blocks;
}

int ut_spdk_bdev_unmap_blocks = 0;
bool ut_spdk_bdev_unmap_blocks_mocked = false;
int
spdk_bdev_unmap_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_unmap_blocks, cb_arg);
	return ut_spdk_bdev_unmap_blocks;
}

int ut_spdk_bdev_flush_blocks = 0;
bool ut_spdk_bdev_flush_blocks_mocked = false;
int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		       void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_flush_blocks, cb_arg);
	return ut_spdk_bdev_flush_blocks;
}

int ut_spdk_bdev_reset = 0;
bool ut_spdk_bdev_reset_mocked = false;
int
spdk_bdev_reset(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	cb(g_bdev_io, !ut_spdk_bdev_reset, cb_arg);
	return ut_spdk_bdev_reset;
}

bool g_completion_called = false;
void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
	g_completion_called = true;
}

int
spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
			   struct iovec *src_iovs, size_t src_iovcnt, uint32_t *output_size, int flags,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{

	return 0;
}

int
spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs, size_t dst_iovcnt,
			     struct iovec *src_iovs, size_t src_iovcnt, uint32_t *output_size, int flags,
			     spdk_accel_completion_cb cb_fn, void *cb_arg)
{

	return 0;
}

/* Global setup for all tests that share a bunch of preparation... */
static int
test_setup(void)
{
	struct spdk_thread *thread;

	spdk_thread_lib_init(NULL, 0);

	thread = spdk_thread_create(NULL, NULL);
	spdk_set_thread(thread);

	g_comp_bdev.reduce_thread = thread;
	g_comp_bdev.backing_dev.unmap = _comp_reduce_unmap;
	g_comp_bdev.backing_dev.readv = _comp_reduce_readv;
	g_comp_bdev.backing_dev.writev = _comp_reduce_writev;
	g_comp_bdev.backing_dev.compress = _comp_reduce_compress;
	g_comp_bdev.backing_dev.decompress = _comp_reduce_decompress;
	g_comp_bdev.backing_dev.blocklen = 512;
	g_comp_bdev.backing_dev.blockcnt = 1024 * 16;
	g_comp_bdev.backing_dev.sgl_in = true;
	g_comp_bdev.backing_dev.sgl_out = true;

	TAILQ_INIT(&g_comp_bdev.queued_comp_ops);

	g_bdev_io = calloc(1, sizeof(struct spdk_bdev_io) + sizeof(struct comp_bdev_io));
	g_bdev_io->u.bdev.iovs = calloc(128, sizeof(struct iovec));
	g_bdev_io->bdev = &g_comp_bdev.comp_bdev;
	g_io_ch = calloc(1, sizeof(struct spdk_io_channel) + sizeof(struct comp_io_channel));
	g_io_ch->thread = thread;
	g_comp_ch = (struct comp_io_channel *)spdk_io_channel_get_ctx(g_io_ch);
	g_io_ctx = (struct comp_bdev_io *)g_bdev_io->driver_ctx;

	g_io_ctx->comp_ch = g_comp_ch;
	g_io_ctx->comp_bdev = &g_comp_bdev;

	return 0;
}

/* Global teardown for all tests */
static int
test_cleanup(void)
{
	struct spdk_thread *thread;

	free(g_bdev_io->u.bdev.iovs);
	free(g_bdev_io);
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
}

static void
test_compress_operation_cross_boundary(void)
{
}

static void
test_vbdev_compress_submit_request(void)
{
	/* Single element block size write */
	g_bdev_io->internal.status = SPDK_BDEV_IO_STATUS_FAILED;
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_WRITE;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);
	CU_ASSERT(g_io_ctx->orig_io == g_bdev_io);
	CU_ASSERT(g_io_ctx->comp_bdev == &g_comp_bdev);
	CU_ASSERT(g_io_ctx->comp_ch == g_comp_ch);

	/* same write but now fail it */
	ut_spdk_reduce_vol_op_complete_err = 1;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);

	/* test a read success */
	g_bdev_io->type = SPDK_BDEV_IO_TYPE_READ;
	ut_spdk_reduce_vol_op_complete_err = 0;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_completion_called == true);

	/* test a read failure */
	ut_spdk_reduce_vol_op_complete_err = 1;
	g_completion_called = false;
	vbdev_compress_submit_request(g_io_ch, g_bdev_io);
	CU_ASSERT(g_bdev_io->internal.status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(g_completion_called == true);
}

static void
test_passthru(void)
{

}

static void
test_reset(void)
{
	/* TODO: There are a few different ways to do this given that
	 * the code uses spdk_for_each_channel() to implement reset
	 * handling. SUbmitting w/o UT for this function for now and
	 * will follow up with something shortly.
	 */
}

static void
test_supported_io(void)
{

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
	CU_ADD_TEST(suite, test_vbdev_compress_submit_request);
	CU_ADD_TEST(suite, test_passthru);
	CU_ADD_TEST(suite, test_supported_io);
	CU_ADD_TEST(suite, test_reset);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
