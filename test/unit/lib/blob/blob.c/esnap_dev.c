/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */
#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "spdk/blob.h"

/*
 * This creates a bs_dev that does not depend on a bdev. Typical use without assertions looks like:
 *
 *	struct spdk_bs_dev	*dev;
 *	struct spdk_bs_opts	bs_opts;
 *	struct spdk_blob_opts	blob_opts;
 *	struct ut_snap_opts	esnap_opts;
 *
 *   Create the blobstore with external snapshot support.
 *	dev = init_dev();
 *	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
 *	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
 *	bs_opts.esnap_bs_dev_create = ut_esnap_create;
 *
 *   Create an esnap clone blob.
 *	ut_spdk_blob_opts_init(&blob_opts);
 *	ut_esnap_opts_init(512, 2048, "name", &esnap_opts);
 *	blob_opts.esnap_id = &esnap_opts;
 *	blob_opts.esnap_id_len = sizeof(esnap_opts);
 *	opts.num_clusters = 4;
 *	blob = ut_blob_create_and_open(bs, &opts);
 *
 * At this point the blob can be used like any other blob.
 */

#define UT_ESNAP_OPTS_MAGIC	0xbadf1ea5
struct ut_esnap_opts {
	/*
	 * This structure gets stored in an xattr. The magic number is used to give some assurance
	 * that we got the right thing before trying to use the other fields.
	 */
	uint32_t	magic;
	uint32_t	block_size;
	uint64_t	num_blocks;
	char		name[32];
};

struct ut_esnap_dev {
	struct spdk_bs_dev	bs_dev;
	struct ut_esnap_opts	ut_opts;
	spdk_blob_id		blob_id;
	uint32_t		num_channels;
};

static void
ut_esnap_opts_init(uint32_t block_size, uint32_t num_blocks, const char *name,
		   struct ut_esnap_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->magic = UT_ESNAP_OPTS_MAGIC;
	opts->block_size = block_size;
	opts->num_blocks = num_blocks;
	spdk_strcpy_pad(opts->name, name, sizeof(opts->name) - 1, '\0');
}

static void
ut_esnap_destroy(struct spdk_bs_dev *bs_dev)
{
	free(bs_dev);
}

static struct spdk_bs_dev *
ut_esnap_dev_alloc(const struct ut_esnap_opts *opts)
{
	struct ut_esnap_dev	*ut_dev;
	struct spdk_bs_dev	*bs_dev;

	assert(opts->magic == UT_ESNAP_OPTS_MAGIC);

	ut_dev = calloc(1, sizeof(*ut_dev));
	if (ut_dev == NULL) {
		return NULL;
	}

	ut_dev->ut_opts = *opts;
	bs_dev = &ut_dev->bs_dev;

	bs_dev->blocklen = opts->block_size;
	bs_dev->blockcnt = opts->num_blocks;

	bs_dev->destroy = ut_esnap_destroy;

	return bs_dev;
}

static int
ut_esnap_create(void *bs_ctx, struct spdk_blob *blob, const void *id, uint32_t id_len,
		struct spdk_bs_dev **bs_devp)
{
	struct spdk_bs_dev	*bs_dev = NULL;

	/* With any blobstore that will use bs_ctx, wrap this function and pass NULL as bs_ctx. */
	CU_ASSERT(bs_ctx == NULL);

	SPDK_CU_ASSERT_FATAL(id != NULL);
	SPDK_CU_ASSERT_FATAL(sizeof(struct ut_esnap_opts) == id_len);

	bs_dev = ut_esnap_dev_alloc(id);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);

	*bs_devp = bs_dev;
	return 0;
}

static int
ut_esnap_create_with_count(void *bs_ctx, struct spdk_blob *blob,
			   const void *id, uint32_t id_len, struct spdk_bs_dev **bs_devp)
{
	uint32_t *bs_ctx_count = bs_ctx;

	SPDK_CU_ASSERT_FATAL(bs_ctx != NULL);

	(*bs_ctx_count)++;
	return ut_esnap_create(NULL, blob, id, id_len, bs_devp);
}
