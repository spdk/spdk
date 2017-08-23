
#include "blob_nvme.h"


struct nvme_blob_bdev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_nvme_ns	*ns;
};


static inline struct spdk_nvme_ns *
__get_ns(struct spdk_bs_dev *dev)
{
	return ((struct nvme_blob_bdev *)dev)->ns;
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
    if (rc)
    {
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
    if (rc)
    {
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
    rc = spdk_nvme_ns_cmd_dataset_management(ns, ctx->qpair, SPDK_NVME_DSM_ATTR_DEALLOCATE, &range, 1, nvme_bdev_blob_io_complete, cb_args);
    if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static struct spdk_io_channel *
nvme_bdev_blob_create_channel(struct spdk_bs_dev *dev)
{
	struct nvme_blob_bdev *blob_bdev = (struct nvme_blob_bdev *)dev;
	SPDK_NOTICELOG("in create_channel\n");
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
nvme_spdk_bdev_create_bs_dev(struct spdk_nvme_ns *ns)
{
	struct nvme_blob_bdev *b;

	b = calloc(1, sizeof(*b));

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