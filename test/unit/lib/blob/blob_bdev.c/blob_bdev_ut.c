/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "blob/bdev/blob_bdev.c"

DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), false);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *g_bdev_io));
DEFINE_STUB(spdk_bdev_queue_io_wait, int,
	    (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
	     struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB(spdk_bdev_read_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_write_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_readv_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_readv_blocks_ext, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_ext, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);
DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, uint64_t offset_blocks,
	     uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_unmap_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, uint64_t offset_blocks,
	     uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_copy_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, uint64_t dst_offset_blocks,
	     uint64_t src_offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *,
	    (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_module_claim_bdev, int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_bdev_module *module),
	    0);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));

struct spdk_bdev {
	char name[16];
	uint64_t blockcnt;
	uint32_t blocklen;
	uint32_t open_cnt;
};

struct spdk_bdev_desc {
	struct spdk_bdev *bdev;
};

struct spdk_bdev *g_bdev;

static struct spdk_bdev *
get_bdev(const char *bdev_name)
{
	if (g_bdev == NULL) {
		return NULL;
	}

	if (strcmp(bdev_name, g_bdev->name) != 0) {
		return NULL;
	}

	return g_bdev;
}

int
spdk_bdev_open_ext(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		   void *event_ctx, struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev = get_bdev(bdev_name);

	if (bdev == NULL) {
		return -ENODEV;
	}

	desc = calloc(1, sizeof(*desc));
	desc->bdev = g_bdev;
	*_desc = desc;
	bdev->open_cnt++;

	return 0;
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev = desc->bdev;

	bdev->open_cnt--;
	free(desc);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
}

static void
init_bdev(struct spdk_bdev *bdev, const char *name, uint64_t num_blocks)
{
	memset(bdev, 0, sizeof(*bdev));
	snprintf(bdev->name, sizeof(bdev->name), "%s", name);
	bdev->blockcnt = num_blocks;
}

static void
create_bs_dev(void)
{
	struct spdk_bdev bdev;
	struct spdk_bs_dev *bs_dev = NULL;
	struct blob_bdev *blob_bdev;
	int rc;

	init_bdev(&bdev, "bdev0", 16);
	g_bdev = &bdev;

	rc = spdk_bdev_create_bs_dev_ext("bdev0", NULL, NULL, &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bdev.open_cnt == 1);

	blob_bdev = (struct blob_bdev *)bs_dev;
	CU_ASSERT(blob_bdev->desc != NULL);
	CU_ASSERT(blob_bdev->desc->bdev == g_bdev);
	CU_ASSERT(!blob_bdev->claimed);

	bs_dev->destroy(bs_dev);
	CU_ASSERT(bdev.open_cnt == 0);
	g_bdev = NULL;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("blob_bdev", NULL, NULL);

	CU_ADD_TEST(suite, create_bs_dev);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
