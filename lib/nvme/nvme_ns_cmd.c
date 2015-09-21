/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

static struct nvme_request *
_nvme_ns_cmd_rw(struct nvme_namespace *ns, void *payload, uint64_t lba,
		uint32_t lba_count, nvme_cb_fn_t cb_fn, void *cb_arg,
		uint32_t opc)
{
	struct nvme_request	*req;
	struct nvme_command	*cmd;
	uint64_t		*tmp_lba;
	uint32_t		sector_size;
	uint32_t		sectors_per_max_io;
	uint32_t		sectors_per_stripe;

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
		uint64_t		remaining_lba_count = lba_count;
		struct nvme_request	*child;

		while (remaining_lba_count > 0) {
			lba_count = sectors_per_stripe - (lba & (sectors_per_stripe - 1));
			lba_count = nvme_min(remaining_lba_count, lba_count);

			child = _nvme_ns_cmd_rw(ns, payload, lba, lba_count, cb_fn,
						cb_arg, opc);
			if (child == NULL) {
				nvme_free_request(req);
				return NULL;
			}
			nvme_request_add_child(req, child);
			remaining_lba_count -= lba_count;
			lba += lba_count;
			payload = (void *)((uintptr_t)payload + (lba_count * sector_size));
		}
	} else if (lba_count > sectors_per_max_io) {
		uint64_t		remaining_lba_count = lba_count;
		struct nvme_request	*child;

		while (remaining_lba_count > 0) {
			lba_count = nvme_min(remaining_lba_count, sectors_per_max_io);
			child = _nvme_ns_cmd_rw(ns, payload, lba, lba_count, cb_fn,
						cb_arg, opc);
			if (child == NULL) {
				nvme_free_request(req);
				return NULL;
			}
			nvme_request_add_child(req, child);
			remaining_lba_count -= lba_count;
			lba += lba_count;
			payload = (void *)((uintptr_t)payload + (lba_count * sector_size));
		}
	} else {
		cmd = &req->cmd;
		cmd->opc = opc;
		cmd->nsid = ns->id;

		tmp_lba = (uint64_t *)&cmd->cdw10;
		*tmp_lba = lba;
		cmd->cdw12 = lba_count - 1;
	}

	return req;
}

int
nvme_ns_cmd_read(struct nvme_namespace *ns, void *payload, uint64_t lba,
		 uint32_t lba_count, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	req = _nvme_ns_cmd_rw(ns, payload, lba, lba_count, cb_fn, cb_arg, NVME_OPC_READ);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
nvme_ns_cmd_write(struct nvme_namespace *ns, void *payload, uint64_t lba,
		  uint32_t lba_count, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	req = _nvme_ns_cmd_rw(ns, payload, lba, lba_count, cb_fn, cb_arg, NVME_OPC_WRITE);
	if (req != NULL) {
		nvme_ctrlr_submit_io_request(ns->ctrlr, req);
		return 0;
	} else {
		return ENOMEM;
	}
}

int
nvme_ns_cmd_deallocate(struct nvme_namespace *ns, void *payload,
		       uint8_t num_ranges, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct nvme_command	*cmd;

	if (num_ranges == 0) {
		return EINVAL;
	}

	req = nvme_allocate_request(payload,
				    num_ranges * sizeof(struct nvme_dsm_range),
				    cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_DATASET_MANAGEMENT;
	cmd->nsid = ns->id;

	/* TODO: create a delete command data structure */
	cmd->cdw10 = num_ranges - 1;
	cmd->cdw11 = NVME_DSM_ATTR_DEALLOCATE;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}

int
nvme_ns_cmd_flush(struct nvme_namespace *ns, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	struct nvme_command	*cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);
	if (req == NULL) {
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_FLUSH;
	cmd->nsid = ns->id;

	nvme_ctrlr_submit_io_request(ns->ctrlr, req);

	return 0;
}
