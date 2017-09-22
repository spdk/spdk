/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/blob_nvme.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/endian.h"

struct nvme_blob_bdev {
	struct spdk_bs_dev bs_dev;
	struct spdk_nvme_ns *ns;
};

static inline struct spdk_nvme_ns *
__get_ns(struct spdk_bs_dev *dev)
{
	return ((struct nvme_blob_bdev *)dev)->ns;
}

static int
blob_nvme_create_cb(void *io_device, void *ctx_buffer)
{
	struct nvme_blob_io_ctx *ch = ctx_buffer;
	struct spdk_nvme_ns *ns = io_device;
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (ch->qpair == NULL) {
		return -ENOMEM;
	} else {
		return 0;
	}
}

static void
blob_nvme_destroy_cb(void *io_device, void *ctx_buffer)
{
	spdk_nvme_ctrlr_free_io_qpair((struct spdk_nvme_qpair *)ctx_buffer);
}

static void
nvme_bdev_blob_io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	int bserrno;
	if (cpl->status.sc == SPDK_NVME_SC_SUCCESS) {
		bserrno = 0;
	} else {
		bserrno = -EIO;
	}
	struct spdk_bs_dev_cb_args *cb_args = arg;
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, bserrno);
}

static void
nvme_bdev_blob_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		    uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;
	struct spdk_nvme_ns *ns = __get_ns(dev);
	struct nvme_blob_io_ctx *ctx = spdk_io_channel_get_ctx(channel);

	rc = spdk_nvme_ns_cmd_read(ns, ctx->qpair, payload, lba,
				   lba_count, nvme_bdev_blob_io_complete, cb_args, 0);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
nvme_bdev_blob_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;
	struct spdk_nvme_ns *ns = __get_ns(dev);
	struct nvme_blob_io_ctx *ctx = spdk_io_channel_get_ctx(channel);

	rc = spdk_nvme_ns_cmd_write(ns, ctx->qpair, payload, lba,
				    lba_count, nvme_bdev_blob_io_complete, cb_args, 0);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
nvme_bdev_blob_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		     uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_nvme_dsm_range range = {};
	struct spdk_nvme_ns *ns = __get_ns(dev);
	struct nvme_blob_io_ctx *ctx = spdk_io_channel_get_ctx(channel);
	int rc;

	range.starting_lba = lba;
	range.length = lba_count;
	rc = spdk_nvme_ns_cmd_dataset_management(ns, ctx->qpair, SPDK_NVME_DSM_ATTR_DEALLOCATE, &range, 1,
			nvme_bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static struct spdk_io_channel *
nvme_bdev_blob_create_channel(struct spdk_bs_dev *dev)
{
	struct nvme_blob_bdev *blob_bdev = (struct nvme_blob_bdev *)dev;
	return spdk_get_io_channel(blob_bdev->ns);
}

static void
nvme_bdev_blob_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
nvme_bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	free(bs_dev);
}

struct spdk_bs_dev *
spdk_bdev_nvme_create_bs_dev(struct spdk_nvme_ns *ns)
{
	struct nvme_blob_bdev *b;

	b = calloc(1, sizeof(*b));

	spdk_io_device_register(ns, blob_nvme_create_cb, blob_nvme_destroy_cb,
				sizeof(struct nvme_blob_io_ctx));

	if (b == NULL) {
		SPDK_ERRLOG("could not allocate nvme_blob_bdev\n");
		return NULL;
	}

	b->ns = ns;
	b->bs_dev.blockcnt = spdk_nvme_ns_get_num_sectors(ns);
	b->bs_dev.blocklen = spdk_nvme_ns_get_sector_size(ns);
	b->bs_dev.create_channel = nvme_bdev_blob_create_channel;
	b->bs_dev.destroy_channel = nvme_bdev_blob_destroy_channel;
	b->bs_dev.destroy = nvme_bdev_blob_destroy;
	b->bs_dev.read = nvme_bdev_blob_read;
	b->bs_dev.write = nvme_bdev_blob_write;
	b->bs_dev.unmap = nvme_bdev_blob_unmap;

	return &b->bs_dev;
}
