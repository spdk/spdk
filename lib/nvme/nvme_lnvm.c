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

#include "nvme_internal.h"

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/nvme_lnvm.h"

bool
spdk_nvme_ctrlr_is_lightnvm_supported(struct spdk_nvme_ctrlr *ctrlr)
{
	if (ctrlr->quirks & NVME_QUIRK_LIGHTNVM) {
		// TODO: There isn't a standardized way to identify Open-Channel SSD
		// different verdors may have different conditions.

		/*
		 * Current QEMU LightNVM Device needs to check nsdata->vs[0].
		 * Here check nsdata->vs[0] of the first namespace.
		 */
		if (ctrlr->cdata.vid == SPDK_PCI_VID_CNEXLABS) {
			if (ctrlr->num_ns && ctrlr->nsdata[0].vendor_specific[0] == 0x1) {
				return true;
			}
		}
	}
	return false;
}

int
spdk_lnvm_cmd_geometry(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
		       void *payload, uint32_t payload_size,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size,
					      cb_fn, cb_arg, false);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_LNVM_OPC_GEOMETRY;
	cmd->nsid = nsid;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
spdk_nvme_ns_lnvm_cmd_vector_reset(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   void *metadata, uint64_t *lbal, uint32_t nlb,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;

	if (nlb == 0 || nlb > 64) {
		return -EINVAL;
	}

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = NULL;
	payload.md = metadata;

	req = nvme_allocate_request(qpair, &payload, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_LNVM_OPC_VECTOR_RESET;
	cmd->nsid = ns->id;
	/*  This is a 0's based value */
	cmd->cdw12 = nlb - 1;
	if (nlb == 1) {
		*(uint64_t *)&cmd->cdw10 = lbal[0];
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lbal);
	}

	return nvme_qpair_submit_request(qpair, req);
}

// TODO: add sgl type rw for vector io

static int
_nvme_ns_lnvm_cmd_vector_rw_with_md(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    void *buffer, void *metadata,
				    uint64_t *lbal, uint32_t nlb,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				    uint32_t opc, uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;
	uint32_t		sector_size;

	if (nlb == 0 || nlb > 64) {
		/* The maximum number of logical blocks is 64 */
		return -EINVAL;
	}
	if (io_flags & 0xFFFF) {
		/* The bottom 16 bits must be empty */
		return -EINVAL;
	}

	// TODO: check ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED
	/*
	 * There is no PI information specially for lnvm in Open-Channel
	 * Spec 2.0. From the definitions of vector commands in OC2.0
	 * and PI in NVMe1.3, it's hard to set PI for vector commands.
	 */

	/* payload setting */
	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = metadata;

	/* request setting */
	sector_size = ns->extended_lba_size;

	req = nvme_allocate_request(qpair, &payload, sector_size * nlb, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}
	req->payload_offset = 0;
	req->md_offset = 0;

	/* cmd setting */
	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;
	/*  This is a 0's based value */
	cmd->cdw12 = nlb - 1;
	cmd->cdw12 |= io_flags;
	if (nlb == 1) {
		*(uint64_t *)&cmd->cdw10 = (uint64_t)lbal[0];
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lbal);
	}

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_lnvm_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lbal, uint32_t nlb,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	return _nvme_ns_lnvm_cmd_vector_rw_with_md(ns, qpair, buffer, metadata,
			lbal, nlb, cb_fn, cb_arg, SPDK_LNVM_OPC_VECTOR_WRITE, io_flags);
}

int
spdk_nvme_ns_lnvm_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lbal, uint32_t nlb,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	return _nvme_ns_lnvm_cmd_vector_rw_with_md(ns, qpair, buffer, metadata,
			lbal, nlb, cb_fn, cb_arg, SPDK_LNVM_OPC_VECTOR_READ, io_flags);
}

int
spdk_nvme_ns_lnvm_cmd_vector_write(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   void *buffer,
				   uint64_t *lbal, uint32_t nlb,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags)
{
	return spdk_nvme_ns_lnvm_cmd_vector_write_with_md(ns, qpair, buffer, NULL,
			lbal, nlb,
			cb_fn, cb_arg,
			io_flags);
}

int
spdk_nvme_ns_lnvm_cmd_vector_read(struct spdk_nvme_ns *ns,
				  struct spdk_nvme_qpair *qpair,
				  void *buffer,
				  uint64_t *lbal, uint32_t nlb,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags)
{
	return spdk_nvme_ns_lnvm_cmd_vector_read_with_md(ns, qpair, buffer, NULL,
			lbal, nlb,
			cb_fn, cb_arg,
			io_flags);
}

int
spdk_nvme_ns_lnvm_cmd_vector_copy(struct spdk_nvme_ns *ns,
				  struct spdk_nvme_qpair *qpair,
				  uint64_t *dlbal, uint64_t *slbal, uint32_t nlb,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (nlb == 0 || nlb > 64) {
		return -EINVAL;
	}
	if (io_flags & 0xFFFF) {
		/* The bottom 16 bits must be empty */
		return -EINVAL;
	}

	/* request setting */
	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}
	req->payload_offset = 0;
	req->md_offset = 0;

	/* cmd setting */
	cmd = &req->cmd;
	cmd->opc = SPDK_LNVM_OPC_VECTOR_COPY;
	cmd->nsid = ns->id;
	/*  This is a 0's based value */
	cmd->cdw12 = nlb - 1;
	cmd->cdw12 |= io_flags;
	if (nlb == 1) {
		*(uint64_t *)&cmd->cdw10 = (uint64_t)slbal[0];
		*(uint64_t *)&cmd->cdw14 = (uint64_t)dlbal[0];
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(slbal);
		*(uint64_t *)&cmd->cdw14 = spdk_vtophys(slbal);
	}

	return nvme_qpair_submit_request(qpair, req);
}
