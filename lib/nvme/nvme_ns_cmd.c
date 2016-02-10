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

#include "nvme_internal.h"

/**
 * \file
 *
 */

static struct nvme_request *_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns,
		const struct nvme_payload *payload, uint64_t lba,
		uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg, uint32_t opc, uint32_t io_flags);

static void
nvme_cb_complete_child(void *child_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *child = child_arg;
	struct nvme_request *parent = child->parent;

	parent->num_children--;
	TAILQ_REMOVE(&parent->children, child, child_tailq);

	if (spdk_nvme_cpl_is_error(cpl)) {
		memcpy(&parent->parent_status, cpl, sizeof(*cpl));
	}

	if (parent->num_children == 0) {
		if (parent->cb_fn) {
			parent->cb_fn(parent->cb_arg, &parent->parent_status);
		}
		nvme_free_request(parent);
	}
}

static void
nvme_request_add_child(struct nvme_request *parent, struct nvme_request *child)
{
	if (parent->num_children == 0) {
		/*
		 * Defer initialization of the children TAILQ since it falls
		 *  on a separate cacheline.  This ensures we do not touch this
		 *  cacheline except on request splitting cases, which are
		 *  relatively rare.
		 */
		TAILQ_INIT(&parent->children);
		parent->parent = NULL;
		memset(&parent->parent_status, 0, sizeof(struct spdk_nvme_cpl));
	}

	parent->num_children++;
	TAILQ_INSERT_TAIL(&parent->children, child, child_tailq);
	child->parent = parent;
	child->cb_fn = nvme_cb_complete_child;
	child->cb_arg = child;
}

static struct nvme_request *
_nvme_ns_cmd_split_request(struct spdk_nvme_ns *ns,
			   const struct nvme_payload *payload,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			   uint32_t io_flags, struct nvme_request *req,
			   uint32_t sectors_per_max_io, uint32_t sector_mask)
{
	uint32_t		sector_size = ns->sector_size;
	uint32_t		remaining_lba_count = lba_count;
	uint32_t		offset = 0;
	struct nvme_request	*child;

	while (remaining_lba_count > 0) {
		lba_count = sectors_per_max_io - (lba & sector_mask);
		lba_count = nvme_min(remaining_lba_count, lba_count);

		child = _nvme_ns_cmd_rw(ns, payload, lba, lba_count, cb_fn,
					cb_arg, opc, io_flags);
		if (child == NULL) {
			nvme_free_request(req);
			return NULL;
		}
		child->payload_offset = offset;
		nvme_request_add_child(req, child);
		remaining_lba_count -= lba_count;
		lba += lba_count;
		offset += lba_count * sector_size;
	}

	return req;
}

static struct nvme_request *
_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns, const struct nvme_payload *payload,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
		uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	uint64_t		*tmp_lba;
	uint32_t		sector_size;
	uint32_t		sectors_per_max_io;
	uint32_t		sectors_per_stripe;

	if (io_flags & 0xFFFF) {
		/* The bottom 16 bits must be empty */
		return NULL;
	}

	sector_size = ns->sector_size;
	sectors_per_max_io = ns->sectors_per_max_io;
	sectors_per_stripe = ns->sectors_per_stripe;

	req = nvme_allocate_request(payload, lba_count * sector_size, cb_fn, cb_arg);
	if (req == NULL) {
		return NULL;
	}

	/*
	 * Intel DC P3*00 NVMe controllers benefit from driver-assisted striping.
	 * If this controller defines a stripe boundary and this I/O spans a stripe
	 *  boundary, split the request into multiple requests and submit each
	 *  separately to hardware.
	 */
	if (sectors_per_stripe > 0 &&
	    (((lba & (sectors_per_stripe - 1)) + lba_count) > sectors_per_stripe)) {

		return _nvme_ns_cmd_split_request(ns, payload, lba, lba_count, cb_fn, cb_arg, opc,
						  io_flags, req, sectors_per_stripe, sectors_per_stripe - 1);
	} else if (lba_count > sectors_per_max_io) {
		return _nvme_ns_cmd_split_request(ns, payload, lba, lba_count, cb_fn, cb_arg, opc,
						  io_flags, req, sectors_per_max_io, 0);
	} else {
		cmd = &req->cmd;
		cmd->opc = opc;
		cmd->nsid = ns->id;

		tmp_lba = (uint64_t *)&cmd->cdw10;
		*tmp_lba = lba;

		cmd->cdw12 = lba_count - 1;
		cmd->cdw12 |= io_flags;
	}

	return req;
}

int
spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, void *buffer, uint64_t lba,
		      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	req = _nvme_ns_cmd_rw(ns, &payload, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ, io_flags);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, uint64_t lba, uint32_t lba_count,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
		       spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	if (reset_sgl_fn == NULL || next_sge_fn == NULL)
		return EINVAL;

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = reset_sgl_fn;
	payload.u.sgl.next_sge_fn = next_sge_fn;

	req = _nvme_ns_cmd_rw(ns, &payload, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ, io_flags);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, void *buffer, uint64_t lba,
		       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	req = _nvme_ns_cmd_rw(ns, &payload, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE, io_flags);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	if (reset_sgl_fn == NULL || next_sge_fn == NULL)
		return EINVAL;

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = reset_sgl_fn;
	payload.u.sgl.next_sge_fn = next_sge_fn;

	req = _nvme_ns_cmd_rw(ns, &payload, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE, io_flags);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, uint64_t lba,
			      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	uint64_t		*tmp_lba;

	if (lba_count == 0) {
		return EINVAL;
	}

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_WRITE_ZEROES;
	cmd->nsid = ns->id;

	tmp_lba = (uint64_t *)&cmd->cdw10;
	*tmp_lba = lba;
	cmd->cdw12 = lba_count - 1;
	cmd->cdw12 |= io_flags;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_deallocate(struct spdk_nvme_ns *ns, void *payload,
			    uint16_t num_ranges, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (num_ranges == 0 || num_ranges > SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES) {
		return EINVAL;
	}

	req = nvme_allocate_request_contig(payload,
					   num_ranges * sizeof(struct spdk_nvme_dsm_range),
					   cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	cmd->nsid = ns->id;

	/* TODO: create a delete command data structure */
	cmd->cdw10 = num_ranges - 1;
	cmd->cdw11 = SPDK_NVME_DSM_ATTR_DEALLOCATE;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_FLUSH;
	cmd->nsid = ns->id;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
				      struct spdk_nvme_reservation_register_data *payload,
				      bool ignore_key,
				      enum spdk_nvme_reservation_register_action action,
				      enum spdk_nvme_reservation_register_cptpl cptpl,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_contig(payload,
					   sizeof(struct spdk_nvme_reservation_register_data),
					   cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_RESERVATION_REGISTER;
	cmd->nsid = ns->id;

	/* Bits 0-2 */
	cmd->cdw10 = action;
	/* Bit 3 */
	cmd->cdw10 |= ignore_key ? 1 << 3 : 0;
	/* Bits 30-31 */
	cmd->cdw10 |= (uint32_t)cptpl << 30;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_reservation_key_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_release_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_contig(payload, sizeof(struct spdk_nvme_reservation_key_data), cb_fn,
					   cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_RESERVATION_RELEASE;
	cmd->nsid = ns->id;

	/* Bits 0-2 */
	cmd->cdw10 = action;
	/* Bit 3 */
	cmd->cdw10 |= ignore_key ? 1 << 3 : 0;
	/* Bits 8-15 */
	cmd->cdw10 |= (uint32_t)type << 8;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_reservation_acquire_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_acquire_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_contig(payload,
					   sizeof(struct spdk_nvme_reservation_acquire_data),
					   cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_RESERVATION_ACQUIRE;
	cmd->nsid = ns->id;

	/* Bits 0-2 */
	cmd->cdw10 = action;
	/* Bit 3 */
	cmd->cdw10 |= ignore_key ? 1 << 3 : 0;
	/* Bits 8-15 */
	cmd->cdw10 |= (uint32_t)type << 8;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns, void *payload,
				    uint32_t len, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t		num_dwords;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (len % 4)
		return EINVAL;
	num_dwords = len / 4;

	req = nvme_allocate_request(payload, num_dwords, cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_RESERVATION_REPORT;
	cmd->nsid = ns->id;

	cmd->cdw10 = num_dwords;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}
