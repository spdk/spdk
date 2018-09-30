/*
 * INTEL CONFIDENTIAL
 *
 * Copyright 2018 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you (License). Unless the License provides otherwise, you may not
 * use, modify, copy, publish, distribute, disclose or transmit this software or
 * the related documents without Intel's prior written permission.
 * This software and the related documents are provided as is, with no express or
 * implied warranties, other than those that are expressly stated in the License.
 */

#ifndef SPDK_OCSSD_CMD_H
#define SPDK_OCSSD_CMD_H

#include "spdk/bdev_target.h"

#ifdef __cplusplus
extern "C" {
#endif

void
spdk_ocssd_req_prep_nsdata(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_ns_data *ns_data, int nsid);

void
spdk_ocssd_req_prep_geometry(struct spdk_bdev_aio_req *req,
		struct spdk_ocssd_geometry_data *geo_data, int nsid);

void
spdk_ocssd_req_prep_chunkinfo(struct spdk_bdev_aio_req *req,
		uint64_t chunk_info_offset, int nchunks,
		struct spdk_ocssd_chunk_information_entry *chks_info, int nsid);

void
spdk_ocssd_req_prep_chunk_reset(struct spdk_bdev_aio_req *req,
		uint64_t ppa, int nsid);

void
spdk_ocssd_req_prep_rw(struct spdk_bdev_aio_req *req,
		uint64_t ppa, uint64_t lba,
		void *data, uint32_t data_len, void *meta, uint32_t md_len,
		uint16_t flags, bool read, int nsid);

/**
 * Read persistent memory data
 *
 * @param req Ptr of req
 * @param buf Buffer to store result of read into, must be aligned to device
 *            granularity min read and size equal to `naddrs *
 *            geo.sector_nbytes`
 * @param length data length
 * @param offset data offset
 * @param flags Access mode
 * @param read Whether it is a read operation
 */
void spdk_ocssd_req_prep_pm_rw(struct spdk_bdev_aio_req *req,
		void *buf, unsigned int length, unsigned int offset,
		uint16_t flags, bool read);

#ifdef __cplusplus
}
#endif

#endif // SPDK_OCSSD_CMD_H
