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

#include "spdk/ocssd_cmd.h"

#define PASSTHRU_CMD_FROM_AIO_REQ(req) (&req->op.passthru.cmd)

void
spdk_ocssd_req_prep_nsdata(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_ns_data *ns_data, int nsid)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);

	memset(req, 0, sizeof(*req));
	cmd->opc = SPDK_NVME_OPC_IDENTIFY;
	cmd->nsid = nsid;

	spdk_bdev_aio_req_prep_admin_passthru(req,
			cmd, (void *)ns_data, sizeof(*ns_data));
}

void
spdk_ocssd_req_prep_geometry(struct spdk_bdev_aio_req *req,
		struct spdk_ocssd_geometry_data *geo_data, int nsid)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);

	memset(req, 0, sizeof(*req));
	cmd->opc = SPDK_OCSSD_OPC_GEOMETRY;
	cmd->nsid = nsid;

	spdk_bdev_aio_req_prep_admin_passthru(req,
			cmd, (void *)geo_data, sizeof(*geo_data));
}

void
spdk_ocssd_req_prep_chunkinfo(struct spdk_bdev_aio_req *req,
		uint64_t chunk_info_offset, int nchunks,
		struct spdk_ocssd_chunk_information_entry *chks_info, int nsid)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);
	uint32_t payload_size, numd;
	uint16_t numdu, numdl;

	memset(req, 0, sizeof(*req));
	cmd->opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd->nsid = nsid;

	//TODO: check whether page_align and buffer_in_boundry is required.
	payload_size = sizeof(*chks_info) * nchunks;
	numd = (payload_size >> 2) - 1;
	numdu = numd >> 16;
	numdl = numd & 0xffff;
	cmd->cdw10 = 0xC4 | (numdl << 16);
	cmd->cdw11 = numdu;

	cmd->cdw12 = chunk_info_offset;
	cmd->cdw13 = chunk_info_offset >> 32;

	spdk_bdev_aio_req_prep_admin_passthru(req,
			cmd, (void *)chks_info, payload_size);
}

void
spdk_ocssd_req_prep_pm_rw(struct spdk_bdev_aio_req *req,
		void *buf, unsigned int length, unsigned int offset,
		uint16_t flags, bool read)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);
	uint32_t payload_size, numd;
	uint16_t numdu, numdl;

	memset(req, 0, sizeof(*req));
	if (read) {
		cmd->opc = 0xCA;
	} else {
		cmd->opc = 0xC9;
	}

	payload_size = length;
	numd = (payload_size >> 2) - 1;
	numdu = numd >> 16;
	numdl = numd & 0xffff;
	cmd->cdw10 = 0x0 | (numdl << 16);
	cmd->cdw11 = numdu;

	spdk_bdev_aio_req_prep_admin_passthru(req,
			cmd, buf, payload_size);
}

void
spdk_ocssd_req_prep_chunk_reset(struct spdk_bdev_aio_req *req,
		uint64_t ppa, int nsid)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);

	memset(req, 0, sizeof(*req));
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_RESET;
	cmd->nsid = nsid;

	cmd->cdw10 = 0x0; // physical reset to free state
	*(uint64_t *)&cmd->cdw14 = ppa;

	spdk_bdev_aio_req_prep_io_passthru(req,
			cmd, NULL, 0, NULL, 0);
}

void
spdk_ocssd_req_prep_rw(struct spdk_bdev_aio_req *req,
		uint64_t ppa, uint64_t lba,
		void *data, uint32_t data_len, void *meta, uint32_t md_len,
		uint16_t flags, bool read, int nsid)
{
	struct spdk_nvme_cmd *cmd = PASSTHRU_CMD_FROM_AIO_REQ(req);

	memset(req, 0, sizeof(*req));
	if (read) {
		cmd->opc = SPDK_NVME_OPC_READ;
	} else {
		cmd->opc = SPDK_NVME_OPC_WRITE;
	}
	cmd->nsid = nsid;

	*(uint64_t *)&cmd->cdw10 = lba;
	*(uint64_t *)&cmd->cdw14 = ppa;

	//TODO: add flag into cmd
	spdk_bdev_aio_req_prep_io_passthru(req,
			cmd, data, data_len, meta, md_len);
}
