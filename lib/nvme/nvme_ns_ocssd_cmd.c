/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/nvme_ocssd.h"
#include "nvme_internal.h"

int
spdk_nvme_ocssd_ns_cmd_vector_reset(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    uint64_t *lba_list, uint32_t num_lbas,
				    struct spdk_ocssd_chunk_information_entry *chunk_info,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	if (!lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_RESET;
	cmd->nsid = ns->id;

	if (chunk_info != NULL) {
		cmd->mptr = spdk_vtophys(chunk_info, NULL);
	}

	/*
	 * Dword 10 and 11 store a pointer to the list of logical block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		*(uint64_t *)&cmd->cdw10 = *lba_list;
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lba_list, NULL);
	}

	cmd->cdw12 = num_lbas - 1;

	return nvme_qpair_submit_request(qpair, req);
}

static int
_nvme_ocssd_ns_cmd_vector_rw_with_md(struct spdk_nvme_ns *ns,
				     struct spdk_nvme_qpair *qpair,
				     void *buffer, void *metadata,
				     uint64_t *lba_list, uint32_t num_lbas,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				     enum spdk_ocssd_io_opcode opc,
				     uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;
	struct nvme_payload	payload;
	uint32_t valid_flags = SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY;

	if (io_flags & ~valid_flags) {
		return -EINVAL;
	}

	if (!buffer || !lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		return -EINVAL;
	}

	payload = NVME_PAYLOAD_CONTIG(buffer, metadata);

	req = nvme_allocate_request(qpair, &payload, num_lbas * ns->sector_size, num_lbas * ns->md_size,
				    cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	/*
	 * Dword 10 and 11 store a pointer to the list of logical block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		*(uint64_t *)&cmd->cdw10 = *lba_list;
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(lba_list, NULL);
	}

	cmd->cdw12 = num_lbas - 1;
	cmd->cdw12 |= io_flags;

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ocssd_ns_cmd_vector_write_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, metadata, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_WRITE, io_flags);
}

int
spdk_nvme_ocssd_ns_cmd_vector_write(struct spdk_nvme_ns *ns,
				    struct spdk_nvme_qpair *qpair,
				    void *buffer,
				    uint64_t *lba_list, uint32_t num_lbas,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				    uint32_t io_flags)
{
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, NULL, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_WRITE, io_flags);
}

int
spdk_nvme_ocssd_ns_cmd_vector_read_with_md(struct spdk_nvme_ns *ns,
		struct spdk_nvme_qpair *qpair,
		void *buffer, void *metadata,
		uint64_t *lba_list, uint32_t num_lbas,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		uint32_t io_flags)
{
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, metadata, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_READ, io_flags);
}

int
spdk_nvme_ocssd_ns_cmd_vector_read(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   void *buffer,
				   uint64_t *lba_list, uint32_t num_lbas,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags)
{
	return _nvme_ocssd_ns_cmd_vector_rw_with_md(ns, qpair, buffer, NULL, lba_list,
			num_lbas, cb_fn, cb_arg, SPDK_OCSSD_OPC_VECTOR_READ, io_flags);
}

int
spdk_nvme_ocssd_ns_cmd_vector_copy(struct spdk_nvme_ns *ns,
				   struct spdk_nvme_qpair *qpair,
				   uint64_t *dst_lba_list,
				   uint64_t *src_lba_list,
				   uint32_t num_lbas,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				   uint32_t io_flags)
{
	struct nvme_request	*req;
	struct spdk_nvme_cmd	*cmd;

	uint32_t valid_flags = SPDK_OCSSD_IO_FLAGS_LIMITED_RETRY;

	if (io_flags & ~valid_flags) {
		return -EINVAL;
	}

	if (!dst_lba_list || !src_lba_list || (num_lbas == 0) ||
	    (num_lbas > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES)) {
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_OCSSD_OPC_VECTOR_COPY;
	cmd->nsid = ns->id;

	/*
	 * Dword 10 and 11 store a pointer to the list of source logical
	 * block addresses.
	 * Dword 14 and 15 store a pointer to the list of destination logical
	 * block addresses.
	 * If there is a single entry in the LBA list, the logical block
	 * address should be stored instead.
	 */
	if (num_lbas == 1) {
		*(uint64_t *)&cmd->cdw10 = *src_lba_list;
		*(uint64_t *)&cmd->cdw14 = *dst_lba_list;
	} else {
		*(uint64_t *)&cmd->cdw10 = spdk_vtophys(src_lba_list, NULL);
		*(uint64_t *)&cmd->cdw14 = spdk_vtophys(dst_lba_list, NULL);
	}

	cmd->cdw12 = num_lbas - 1;
	cmd->cdw12 |= io_flags;

	return nvme_qpair_submit_request(qpair, req);
}
