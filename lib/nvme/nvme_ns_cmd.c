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

static struct nvme_request *_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns,
		const struct nvme_payload *payload, uint32_t payload_offset, uint32_t md_offset,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
		void *cb_arg, uint32_t opc, uint32_t io_flags,
		uint16_t apptag_mask, uint16_t apptag);

static void
nvme_cb_complete_child(void *child_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request *child = child_arg;
	struct nvme_request *parent = child->parent;

	nvme_request_remove_child(parent, child);

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

void
nvme_request_remove_child(struct nvme_request *parent, struct nvme_request *child)
{
	assert(parent != NULL);
	assert(child != NULL);
	assert(child->parent == parent);
	assert(parent->num_children != 0);

	parent->num_children--;
	TAILQ_REMOVE(&parent->children, child, child_tailq);
}

static void
nvme_request_free_children(struct nvme_request *req)
{
	struct nvme_request *child, *tmp;

	if (req->num_children == 0) {
		return;
	}

	/* free all child nvme_request */
	TAILQ_FOREACH_SAFE(child, &req->children, child_tailq, tmp) {
		nvme_request_remove_child(req, child);
		nvme_free_request(child);
	}
}

static struct nvme_request *
_nvme_ns_cmd_split_request(struct spdk_nvme_ns *ns,
			   const struct nvme_payload *payload,
			   uint32_t payload_offset, uint32_t md_offset,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
			   uint32_t io_flags, struct nvme_request *req,
			   uint32_t sectors_per_max_io, uint32_t sector_mask,
			   uint16_t apptag_mask, uint16_t apptag)
{
	uint32_t		sector_size = ns->sector_size;
	uint32_t		md_size = ns->md_size;
	uint32_t		remaining_lba_count = lba_count;
	struct nvme_request	*child;

	if (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		/* for extended LBA only */
		if ((ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) && !(io_flags & SPDK_NVME_IO_FLAGS_PRACT))
			sector_size += ns->md_size;
	}

	while (remaining_lba_count > 0) {
		lba_count = sectors_per_max_io - (lba & sector_mask);
		lba_count = nvme_min(remaining_lba_count, lba_count);

		child = _nvme_ns_cmd_rw(ns, payload, payload_offset, md_offset, lba, lba_count, cb_fn,
					cb_arg, opc, io_flags, apptag_mask, apptag);
		if (child == NULL) {
			nvme_request_free_children(req);
			return NULL;
		}

		nvme_request_add_child(req, child);
		remaining_lba_count -= lba_count;
		lba += lba_count;
		payload_offset += lba_count * sector_size;
		md_offset += lba_count * md_size;
	}

	return req;
}

static void
_nvme_ns_cmd_setup_request(struct spdk_nvme_ns *ns, struct nvme_request *req,
			   uint32_t opc, uint32_t lba, uint32_t lba_count,
			   uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct spdk_nvme_cmd	*cmd;

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	*(uint64_t *)&cmd->cdw10 = lba;

	if (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		switch (ns->pi_type) {
		case SPDK_NVME_FMT_NVM_PROTECTION_TYPE1:
		case SPDK_NVME_FMT_NVM_PROTECTION_TYPE2:
			cmd->cdw14 = (uint32_t)lba;
			break;
		}
	}

	cmd->cdw12 = lba_count - 1;
	cmd->cdw12 |= io_flags;

	cmd->cdw15 = apptag_mask;
	cmd->cdw15 = (cmd->cdw15 << 16 | apptag);
}

static struct nvme_request *
_nvme_ns_cmd_rw(struct spdk_nvme_ns *ns, const struct nvme_payload *payload,
		uint32_t payload_offset, uint32_t md_offset,
		uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t opc,
		uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request	*req;
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

	if (ns->flags & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		/* for extended LBA only */
		if ((ns->flags & SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED) && !(io_flags & SPDK_NVME_IO_FLAGS_PRACT))
			sector_size += ns->md_size;
	}

	req = nvme_allocate_request(payload, lba_count * sector_size, cb_fn, cb_arg);
	if (req == NULL) {
		return NULL;
	}

	req->payload_offset = payload_offset;
	req->md_offset = md_offset;

	/*
	 * Intel DC P3*00 NVMe controllers benefit from driver-assisted striping.
	 * If this controller defines a stripe boundary and this I/O spans a stripe
	 *  boundary, split the request into multiple requests and submit each
	 *  separately to hardware.
	 */
	if (sectors_per_stripe > 0 &&
	    (((lba & (sectors_per_stripe - 1)) + lba_count) > sectors_per_stripe)) {

		return _nvme_ns_cmd_split_request(ns, payload, payload_offset, md_offset, lba, lba_count, cb_fn,
						  cb_arg, opc,
						  io_flags, req, sectors_per_stripe, sectors_per_stripe - 1, apptag_mask, apptag);
	} else if (lba_count > sectors_per_max_io) {
		return _nvme_ns_cmd_split_request(ns, payload, payload_offset, md_offset, lba, lba_count, cb_fn,
						  cb_arg, opc,
						  io_flags, req, sectors_per_max_io, 0, apptag_mask, apptag);
	}

	_nvme_ns_cmd_setup_request(ns, req, opc, lba, lba_count, io_flags, apptag_mask, apptag);
	return req;
}

int
spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
		      uint64_t lba,
		      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = NULL;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags, 0,
			      0);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_read_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *buffer,
			      void *metadata,
			      uint64_t lba,
			      uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = metadata;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags,
			      apptag_mask, apptag);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint64_t lba, uint32_t lba_count,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
		       spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
		       spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	if (reset_sgl_fn == NULL || next_sge_fn == NULL)
		return -EINVAL;

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.md = NULL;
	payload.u.sgl.reset_sgl_fn = reset_sgl_fn;
	payload.u.sgl.next_sge_fn = next_sge_fn;
	payload.u.sgl.cb_arg = cb_arg;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_READ,
			      io_flags, 0, 0);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       void *buffer, uint64_t lba,
		       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = NULL;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, 0, 0);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_write_with_md(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			       void *buffer, void *metadata, uint64_t lba,
			       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			       uint32_t io_flags, uint16_t apptag_mask, uint16_t apptag)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;
	payload.md = metadata;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, apptag_mask, apptag);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			uint64_t lba, uint32_t lba_count,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			spdk_nvme_req_next_sge_cb next_sge_fn)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	if (reset_sgl_fn == NULL || next_sge_fn == NULL)
		return -EINVAL;

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.md = NULL;
	payload.u.sgl.reset_sgl_fn = reset_sgl_fn;
	payload.u.sgl.next_sge_fn = next_sge_fn;
	payload.u.sgl.cb_arg = cb_arg;

	req = _nvme_ns_cmd_rw(ns, &payload, 0, 0, lba, lba_count, cb_fn, cb_arg, SPDK_NVME_OPC_WRITE,
			      io_flags, 0, 0);
	if (req != NULL) {
		return nvme_qpair_submit_request(qpair, req);
	} else {
		return -ENOMEM;
	}
}

int
spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			      uint64_t lba, uint32_t lba_count,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
			      uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	uint64_t		*tmp_lba;

	if (lba_count == 0) {
		return -EINVAL;
	}

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_WRITE_ZEROES;
	cmd->nsid = ns->id;

	tmp_lba = (uint64_t *)&cmd->cdw10;
	*tmp_lba = lba;
	cmd->cdw12 = lba_count - 1;
	cmd->cdw12 |= io_flags;

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_dataset_management(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
				    uint32_t type,
				    const struct spdk_nvme_dsm_range *ranges, uint16_t num_ranges,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (num_ranges == 0 || num_ranges > SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES) {
		return -EINVAL;
	}

	if (ranges == NULL) {
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy((void *)ranges,
					      num_ranges * sizeof(struct spdk_nvme_dsm_range),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	cmd->nsid = ns->id;

	cmd->cdw10 = num_ranges - 1;
	cmd->cdw11 = type;

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_FLUSH;
	cmd->nsid = ns->id;

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
				      struct spdk_nvme_qpair *qpair,
				      struct spdk_nvme_reservation_register_data *payload,
				      bool ignore_key,
				      enum spdk_nvme_reservation_register_action action,
				      enum spdk_nvme_reservation_register_cptpl cptpl,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_user_copy(payload, sizeof(struct spdk_nvme_reservation_register_data),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		return -ENOMEM;
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

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     struct spdk_nvme_reservation_key_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_release_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_user_copy(payload, sizeof(struct spdk_nvme_reservation_key_data), cb_fn,
					      cb_arg, true);
	if (req == NULL) {
		return -ENOMEM;
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

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     struct spdk_nvme_reservation_acquire_data *payload,
				     bool ignore_key,
				     enum spdk_nvme_reservation_acquire_action action,
				     enum spdk_nvme_reservation_type type,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	req = nvme_allocate_request_user_copy(payload, sizeof(struct spdk_nvme_reservation_acquire_data),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		return -ENOMEM;
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

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    void *payload, uint32_t len,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t		num_dwords;
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (len % 4)
		return -EINVAL;
	num_dwords = len / 4;

	req = nvme_allocate_request_user_copy(payload, len, cb_fn, cb_arg, false);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_RESERVATION_REPORT;
	cmd->nsid = ns->id;

	cmd->cdw10 = num_dwords;

	return nvme_qpair_submit_request(qpair, req);
}
