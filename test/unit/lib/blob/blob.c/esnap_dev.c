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
 *	struct spdk_io_channel	*bs_chan;
 *	bool			destroyed = false;
 *
 *   Create the blobstore with external snapshot support.
 *	dev = init_dev();
 *	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
 *	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
 *	bs_opts.esnap_bs_dev_create = ut_esnap_create;
 *
 *   Create an esnap clone blob.
 *	ut_esnap_opts_init(512, 2048, "name", &destroyed, &esnap_opts);
 *	blob_opts.esnap_id = &esnap_opts;
 *	blob_opts.esnap_id_len = sizeof(esnap_opts);
 *	opts.num_clusters = 4;
 *	blob = ut_blob_create_and_open(bs, &opts);
 *
 *   Do stuff like you would with any other blob.
 *	bs_chan = spdk_bs_alloc_io_channel(bs);
 *	...
 *
 *   You can check the value of destroyed to verify that spdk_blob_close() led to the
 *   destruction of the bs_dev created during spdk_blob_open().
 *	spdk_blob_close(blob, blob_op_complete, NULL);
 *	poll_threads();
 *	CU_ASSERT(destroyed);
 */

static void
ut_memset4(void *dst, uint32_t pat, size_t len)
{
	uint32_t *vals = dst;

	assert((len % 4) == 0);
	for (size_t i = 0; i < (len / 4); i++) {
		vals[i] = pat;
	}
}

static void
ut_memset8(void *dst, uint64_t pat, size_t len)
{
	uint64_t *vals = dst;

	assert((len % 8) == 0);
	for (size_t i = 0; i < (len / 8); i++) {
		vals[i] = pat;
	}
}

#define UT_ESNAP_OPTS_MAGIC	0xbadf1ea5
struct ut_esnap_opts {
	/*
	 * This structure gets stored in an xattr. The magic number is used to give some assurance
	 * that we got the right thing before trying to use the other fields.
	 */
	uint32_t	magic;
	uint32_t	block_size;
	uint64_t	num_blocks;
	/*
	 * If non-NULL, referenced address will be set to true when the device is fully destroyed.
	 * This address must remain valid for the life of the blob, even across blobstore reload.
	 */
	bool		*destroyed;
	char		name[32];
};

struct ut_esnap_dev {
	struct spdk_bs_dev	bs_dev;
	struct ut_esnap_opts	ut_opts;
	spdk_blob_id		blob_id;
	uint32_t		num_channels;
};

struct ut_esnap_channel {
	struct ut_esnap_dev	*dev;
	struct spdk_thread	*thread;
	uint64_t		blocks_read;
};

static void
ut_esnap_opts_init(uint32_t block_size, uint32_t num_blocks, const char *name, bool *destroyed,
		   struct ut_esnap_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->magic = UT_ESNAP_OPTS_MAGIC;
	opts->block_size = block_size;
	opts->num_blocks = num_blocks;
	opts->destroyed = destroyed;
	spdk_strcpy_pad(opts->name, name, sizeof(opts->name) - 1, '\0');
}

static struct spdk_io_channel *
ut_esnap_create_channel(struct spdk_bs_dev *dev)
{
	struct spdk_io_channel *ch;

	ch = spdk_get_io_channel(dev);
	if (ch == NULL) {
		return NULL;
	}

	return ch;
}

static void
ut_esnap_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

/*
 * When reading, each block is filled with 64-bit values made up of the least significant 32 bits of
 * the blob ID and the lba.
 */
union ut_word {
	uint64_t	num;
	struct {
		uint32_t	blob_id;
		uint32_t	lba;
	} f;
};

static bool
ut_esnap_content_is_correct(void *buf, uint32_t buf_sz, uint32_t id,
			    uint32_t start_byte, uint32_t esnap_blksz)
{
	union ut_word	*words = buf;
	uint32_t	off, i, j, lba;

	j = 0;
	for (off = start_byte; off < start_byte + buf_sz; off += esnap_blksz) {
		lba = off / esnap_blksz;
		for (i = 0; i < esnap_blksz / sizeof(*words); i++) {
			if (words[j].f.blob_id != id || words[j].f.lba != lba) {
				return false;
			}
			j++;
		}
	}
	return true;
}

static void
ut_esnap_read(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel, void *payload,
	      uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct ut_esnap_dev	*ut_dev = (struct ut_esnap_dev *)bs_dev;
	struct ut_esnap_channel	*ut_ch = spdk_io_channel_get_ctx(channel);
	const uint32_t		block_size = ut_dev->ut_opts.block_size;
	union ut_word		word;
	uint64_t		cur;

	/* The channel passed in must be associated with this bs_dev. */
	CU_ASSERT(&ut_ch->dev->bs_dev == bs_dev);
	CU_ASSERT(spdk_get_thread() == ut_ch->thread);

	SPDK_CU_ASSERT_FATAL(sizeof(word) == 8);
	SPDK_CU_ASSERT_FATAL(lba + lba_count <= UINT32_MAX);

	word.f.blob_id = ut_dev->blob_id & 0xffffffff;
	for (cur = 0; cur < lba_count; cur++) {
		word.f.lba = lba + cur;
		ut_memset8(payload + cur * block_size, word.num, block_size);
	}
	ut_ch->blocks_read += lba_count;

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
ut_esnap_readv(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel,
	       struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	       struct spdk_bs_dev_cb_args *cb_args)
{
	struct ut_esnap_channel	*ut_ch = spdk_io_channel_get_ctx(channel);

	/* The channel passed in must be associated with this bs_dev. */
	CU_ASSERT(&ut_ch->dev->bs_dev == bs_dev);
	CU_ASSERT(spdk_get_thread() == ut_ch->thread);

	if (iovcnt != 1) {
		CU_ASSERT(false);
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
		return;
	}
	ut_esnap_read(bs_dev, channel, iov->iov_base, lba, lba_count, cb_args);
}

static void
ut_esnap_readv_ext(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		   struct spdk_bs_dev_cb_args *cb_args, struct spdk_blob_ext_io_opts *io_opts)
{
	struct ut_esnap_channel	*ut_ch = spdk_io_channel_get_ctx(channel);

	/* The channel passed in must be associated with this bs_dev. */
	CU_ASSERT(&ut_ch->dev->bs_dev == bs_dev);
	CU_ASSERT(spdk_get_thread() == ut_ch->thread);

	CU_ASSERT(false);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static bool
ut_esnap_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return false;
}

static int
ut_esnap_io_channel_create(void *io_device, void *ctx)
{
	struct ut_esnap_dev	*ut_dev = io_device;
	struct ut_esnap_channel	*ut_ch = ctx;

	ut_ch->dev = ut_dev;
	ut_ch->thread = spdk_get_thread();
	ut_ch->blocks_read = 0;

	ut_dev->num_channels++;

	return 0;
}

static void
ut_esnap_io_channel_destroy(void *io_device, void *ctx)
{
	struct ut_esnap_dev	*ut_dev = io_device;
	struct ut_esnap_channel	*ut_ch = ctx;

	CU_ASSERT(ut_ch->thread == spdk_get_thread());

	CU_ASSERT(ut_dev->num_channels > 0);
	ut_dev->num_channels--;

	return;
}

static void
ut_esnap_dev_free(void *io_device)
{
	struct ut_esnap_dev	*ut_dev = io_device;

	if (ut_dev->ut_opts.destroyed != NULL) {
		*ut_dev->ut_opts.destroyed = true;
	}

	CU_ASSERT(ut_dev->num_channels == 0);

	ut_memset4(ut_dev, 0xdeadf1ea, sizeof(*ut_dev));
	free(ut_dev);
}

static void
ut_esnap_destroy(struct spdk_bs_dev *bs_dev)
{
	spdk_io_device_unregister(bs_dev, ut_esnap_dev_free);
}

static bool
ut_esnap_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	*base_lba = lba;
	return true;
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

	bs_dev->create_channel = ut_esnap_create_channel;
	bs_dev->destroy_channel = ut_esnap_destroy_channel;
	bs_dev->destroy = ut_esnap_destroy;
	bs_dev->read = ut_esnap_read;
	bs_dev->readv = ut_esnap_readv;
	bs_dev->readv_ext = ut_esnap_readv_ext;
	bs_dev->is_zeroes = ut_esnap_is_zeroes;
	bs_dev->translate_lba = ut_esnap_translate_lba;

	spdk_io_device_register(ut_dev, ut_esnap_io_channel_create, ut_esnap_io_channel_destroy,
				sizeof(struct ut_esnap_channel), opts->name);

	return bs_dev;
}

static int
ut_esnap_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
		const void *id, uint32_t id_len, struct spdk_bs_dev **bs_devp)
{
	struct spdk_bs_dev	*bs_dev = NULL;

	/* With any blobstore that will use bs_ctx or blob_ctx, wrap this function and pass NULL as
	 * bs_ctx and blob_ctx. */
	CU_ASSERT(bs_ctx == NULL);
	CU_ASSERT(bs_ctx == NULL);

	SPDK_CU_ASSERT_FATAL(id != NULL);
	SPDK_CU_ASSERT_FATAL(sizeof(struct ut_esnap_opts) == id_len);

	bs_dev = ut_esnap_dev_alloc(id);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);

	*bs_devp = bs_dev;
	return 0;
}

static int
ut_esnap_create_with_count(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			   const void *id, uint32_t id_len, struct spdk_bs_dev **bs_devp)
{
	uint32_t *bs_ctx_count = bs_ctx;
	uint32_t *blob_ctx_count = blob_ctx;

	SPDK_CU_ASSERT_FATAL(bs_ctx != NULL);

	(*bs_ctx_count)++;

	/*
	 * blob_ctx can be non-NULL when spdk_bs_open_blob() is used. Opens that come via
	 * spdk_bs_load(), spdk_bs_open_blob(), and those that come via spdk_bs_open_blob_ext() with
	 * NULL opts->esnap_ctx will have blob_ctx == NULL.
	 */
	if (blob_ctx_count != NULL) {
		(*blob_ctx_count)++;
	}

	return ut_esnap_create(NULL, NULL, blob, id, id_len, bs_devp);
}

static struct ut_esnap_channel *
ut_esnap_get_io_channel(struct spdk_io_channel *ch, spdk_blob_id blob_id)
{
	struct spdk_bs_channel	*bs_channel = spdk_io_channel_get_ctx(ch);
	struct blob_esnap_channel	find = {};
	struct blob_esnap_channel	*esnap_channel;

	find.blob_id = blob_id;
	esnap_channel = RB_FIND(blob_esnap_channel_tree, &bs_channel->esnap_channels, &find);
	if (esnap_channel == NULL) {
		return NULL;
	}

	return spdk_io_channel_get_ctx(esnap_channel->channel);
}
