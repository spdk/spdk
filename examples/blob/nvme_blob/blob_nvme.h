


#ifndef BLOB_NVME_H
#define BLOB_NVME_H

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/endian.h"

struct nvme_blob_io_ctx {
    struct spdk_nvme_qpair *qpair;
};

struct spdk_bs_dev *nvme_spdk_bdev_create_bs_dev(struct spdk_nvme_ns *ns);

#endif
