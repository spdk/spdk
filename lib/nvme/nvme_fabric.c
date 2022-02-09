/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

/*
 * NVMe over Fabrics transport-independent functions
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/string.h"

struct nvme_fabric_prop_ctx {
	uint64_t		value;
	int			size;
	spdk_nvme_reg_cb	cb_fn;
	void			*cb_arg;
};

static int
nvme_fabric_prop_set_cmd(struct spdk_nvme_ctrlr *ctrlr,
			 uint32_t offset, uint8_t size, uint64_t value,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
	cmd.ofst = offset;
	cmd.attrib.size = size;
	cmd.value.u64 = value;

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					     NULL, 0, cb_fn, cb_arg);
}

static int
nvme_fabric_prop_set_cmd_sync(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t value)
{
	struct nvme_completion_poll_status *status;
	int rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = nvme_fabric_prop_set_cmd(ctrlr, offset, size, value,
				      nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		SPDK_ERRLOG("Property Set failed\n");
		return -1;
	}
	free(status);

	return 0;
}

static void
nvme_fabric_prop_set_cmd_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fabric_prop_ctx *prop_ctx = ctx;

	prop_ctx->cb_fn(prop_ctx->cb_arg, prop_ctx->value, cpl);
	free(prop_ctx);
}

static int
nvme_fabric_prop_set_cmd_async(struct spdk_nvme_ctrlr *ctrlr,
			       uint32_t offset, uint8_t size, uint64_t value,
			       spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	struct nvme_fabric_prop_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate fabrics property context\n");
		return -ENOMEM;
	}

	ctx->value = value;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = nvme_fabric_prop_set_cmd(ctrlr, offset, size, value,
				      nvme_fabric_prop_set_cmd_done, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send Property Set fabrics command\n");
		free(ctx);
	}

	return rc;
}

static int
nvme_fabric_prop_get_cmd(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint8_t size,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	cmd.ofst = offset;
	cmd.attrib.size = size;

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					     NULL, 0, cb_fn, cb_arg);
}

static int
nvme_fabric_prop_get_cmd_sync(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t *value)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	int rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = nvme_fabric_prop_get_cmd(ctrlr, offset, size, nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		SPDK_ERRLOG("Property Get failed\n");
		return -1;
	}

	response = (struct spdk_nvmf_fabric_prop_get_rsp *)&status->cpl;

	if (size == SPDK_NVMF_PROP_SIZE_4) {
		*value = response->value.u32.low;
	} else {
		*value = response->value.u64;
	}

	free(status);

	return 0;
}

static void
nvme_fabric_prop_get_cmd_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fabric_prop_ctx *prop_ctx = ctx;
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	uint64_t value = 0;

	if (spdk_nvme_cpl_is_success(cpl)) {
		response = (struct spdk_nvmf_fabric_prop_get_rsp *)cpl;

		switch (prop_ctx->size) {
		case SPDK_NVMF_PROP_SIZE_4:
			value = response->value.u32.low;
			break;
		case SPDK_NVMF_PROP_SIZE_8:
			value = response->value.u64;
			break;
		default:
			assert(0 && "Should never happen");
		}
	}

	prop_ctx->cb_fn(prop_ctx->cb_arg, value, cpl);
	free(prop_ctx);
}

static int
nvme_fabric_prop_get_cmd_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint8_t size,
			       spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	struct nvme_fabric_prop_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate fabrics property context\n");
		return -ENOMEM;
	}

	ctx->size = size;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = nvme_fabric_prop_get_cmd(ctrlr, offset, size, nvme_fabric_prop_get_cmd_done, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send Property Get fabrics command\n");
		free(ctx);
	}

	return rc;
}

int
nvme_fabric_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_fabric_prop_set_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value);
}

int
nvme_fabric_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_fabric_prop_set_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

int
nvme_fabric_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	uint64_t tmp_value;
	int rc;
	rc = nvme_fabric_prop_get_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, &tmp_value);

	if (!rc) {
		*value = (uint32_t)tmp_value;
	}
	return rc;
}

int
nvme_fabric_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_fabric_prop_get_cmd_sync(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

int
nvme_fabric_ctrlr_set_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  uint32_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_set_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value,
					      cb_fn, cb_arg);
}

int
nvme_fabric_ctrlr_set_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  uint64_t value, spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_set_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value,
					      cb_fn, cb_arg);
}

int
nvme_fabric_ctrlr_get_reg_4_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_get_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, cb_fn, cb_arg);
}

int
nvme_fabric_ctrlr_get_reg_8_async(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset,
				  spdk_nvme_reg_cb cb_fn, void *cb_arg)
{
	return nvme_fabric_prop_get_cmd_async(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, cb_fn, cb_arg);
}

static void
nvme_fabric_discover_probe(struct spdk_nvmf_discovery_log_page_entry *entry,
			   struct spdk_nvme_probe_ctx *probe_ctx,
			   int discover_priority)
{
	struct spdk_nvme_transport_id trid;
	uint8_t *end;
	size_t len;

	memset(&trid, 0, sizeof(trid));

	if (entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		SPDK_WARNLOG("Skipping unsupported discovery service referral\n");
		return;
	} else if (entry->subtype != SPDK_NVMF_SUBTYPE_NVME) {
		SPDK_WARNLOG("Skipping unknown subtype %u\n", entry->subtype);
		return;
	}

	trid.trtype = entry->trtype;
	spdk_nvme_transport_id_populate_trstring(&trid, spdk_nvme_transport_id_trtype_str(entry->trtype));
	if (!spdk_nvme_transport_available_by_name(trid.trstring)) {
		SPDK_WARNLOG("NVMe transport type %u not available; skipping probe\n",
			     trid.trtype);
		return;
	}

	trid.adrfam = entry->adrfam;

	/* Ensure that subnqn is null terminated. */
	end = memchr(entry->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Discovery entry SUBNQN is not null terminated\n");
		return;
	}
	len = end - entry->subnqn;
	memcpy(trid.subnqn, entry->subnqn, len);
	trid.subnqn[len] = '\0';

	/* Convert traddr to a null terminated string. */
	len = spdk_strlen_pad(entry->traddr, sizeof(entry->traddr), ' ');
	memcpy(trid.traddr, entry->traddr, len);
	if (spdk_str_chomp(trid.traddr) != 0) {
		SPDK_DEBUGLOG(nvme, "Trailing newlines removed from discovery TRADDR\n");
	}

	/* Convert trsvcid to a null terminated string. */
	len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
	memcpy(trid.trsvcid, entry->trsvcid, len);
	if (spdk_str_chomp(trid.trsvcid) != 0) {
		SPDK_DEBUGLOG(nvme, "Trailing newlines removed from discovery TRSVCID\n");
	}

	SPDK_DEBUGLOG(nvme, "subnqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n",
		      trid.subnqn, trid.trtype,
		      trid.traddr, trid.trsvcid);

	/* Copy the priority from the discovery ctrlr */
	trid.priority = discover_priority;

	nvme_ctrlr_probe(&trid, probe_ctx, NULL);
}

static int
nvme_fabric_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
				   void *log_page, uint32_t size, uint64_t offset)
{
	struct nvme_completion_poll_status *status;
	int rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0, log_page, size, offset,
					      nvme_completion_poll_cb, status);
	if (rc < 0) {
		free(status);
		return -1;
	}

	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}
	free(status);

	return 0;
}

int
nvme_fabric_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		       bool direct_connect)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	int rc;
	struct nvme_completion_poll_status *status;

	if (strcmp(probe_ctx->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
		/* It is not a discovery_ctrlr info and try to directly connect it */
		rc = nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
		return rc;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&discovery_opts, sizeof(discovery_opts));
	if (direct_connect && probe_ctx->probe_cb) {
		probe_ctx->probe_cb(probe_ctx->cb_ctx, &probe_ctx->trid, &discovery_opts);
	}

	discovery_ctrlr = nvme_transport_ctrlr_construct(&probe_ctx->trid, &discovery_opts, NULL);
	if (discovery_ctrlr == NULL) {
		return -1;
	}

	while (discovery_ctrlr->state != NVME_CTRLR_STATE_READY) {
		if (nvme_ctrlr_process_init(discovery_ctrlr) != 0) {
			nvme_ctrlr_destruct(discovery_ctrlr);
			return -1;
		}
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		return -ENOMEM;
	}

	/* get the cdata info */
	rc = nvme_ctrlr_cmd_identify(discovery_ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0, 0,
				     &discovery_ctrlr->cdata, sizeof(discovery_ctrlr->cdata),
				     nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to identify cdata\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion(discovery_ctrlr->adminq, status)) {
		SPDK_ERRLOG("nvme_identify_controller failed!\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		if (!status->timed_out) {
			free(status);
		}
		return -ENXIO;
	}

	free(status);

	/* Direct attach through spdk_nvme_connect() API */
	if (direct_connect == true) {
		/* Set the ready state to skip the normal init process */
		discovery_ctrlr->state = NVME_CTRLR_STATE_READY;
		nvme_ctrlr_connected(probe_ctx, discovery_ctrlr);
		nvme_ctrlr_add_process(discovery_ctrlr, 0);
		return 0;
	}

	rc = nvme_fabric_ctrlr_discover(discovery_ctrlr, probe_ctx);
	nvme_ctrlr_destruct(discovery_ctrlr);
	return rc;
}

int
nvme_fabric_ctrlr_discover(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvmf_discovery_log_page *log_page;
	struct spdk_nvmf_discovery_log_page_entry *log_page_entry;
	char buffer[4096];
	int rc;
	uint64_t i, numrec, buffer_max_entries_first, buffer_max_entries, log_page_offset = 0;
	uint64_t remaining_num_rec = 0;
	uint16_t recfmt;

	memset(buffer, 0x0, 4096);
	buffer_max_entries_first = (sizeof(buffer) - offsetof(struct spdk_nvmf_discovery_log_page,
				    entries[0])) /
				   sizeof(struct spdk_nvmf_discovery_log_page_entry);
	buffer_max_entries = sizeof(buffer) / sizeof(struct spdk_nvmf_discovery_log_page_entry);
	do {
		rc = nvme_fabric_get_discovery_log_page(ctrlr, buffer, sizeof(buffer), log_page_offset);
		if (rc < 0) {
			SPDK_DEBUGLOG(nvme, "Get Log Page - Discovery error\n");
			return rc;
		}

		if (!remaining_num_rec) {
			log_page = (struct spdk_nvmf_discovery_log_page *)buffer;
			recfmt = from_le16(&log_page->recfmt);
			if (recfmt != 0) {
				SPDK_ERRLOG("Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
				return -EPROTO;
			}
			remaining_num_rec = log_page->numrec;
			log_page_offset = offsetof(struct spdk_nvmf_discovery_log_page, entries[0]);
			log_page_entry = &log_page->entries[0];
			numrec = spdk_min(remaining_num_rec, buffer_max_entries_first);
		} else {
			numrec = spdk_min(remaining_num_rec, buffer_max_entries);
			log_page_entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
		}

		for (i = 0; i < numrec; i++) {
			nvme_fabric_discover_probe(log_page_entry++, probe_ctx, ctrlr->trid.priority);
		}
		remaining_num_rec -= numrec;
		log_page_offset += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	} while (remaining_num_rec != 0);

	return 0;
}

int
nvme_fabric_qpair_connect_async(struct spdk_nvme_qpair *qpair, uint32_t num_entries)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvmf_fabric_connect_cmd cmd;
	struct spdk_nvmf_fabric_connect_data *nvmf_data;
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_request *req;
	int rc;

	if (num_entries == 0 || num_entries > SPDK_NVME_IO_QUEUE_MAX_ENTRIES) {
		return -EINVAL;
	}

	ctrlr = qpair->ctrlr;
	if (!ctrlr) {
		return -EINVAL;
	}

	nvmf_data = spdk_zmalloc(sizeof(*nvmf_data), 0, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!nvmf_data) {
		SPDK_ERRLOG("nvmf_data allocation error\n");
		return -ENOMEM;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		spdk_free(nvmf_data);
		return -ENOMEM;
	}

	status->dma_data = nvmf_data;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.qid = qpair->id;
	cmd.sqsize = num_entries - 1;
	cmd.kato = ctrlr->opts.keep_alive_timeout_ms;

	assert(qpair->reserved_req != NULL);
	req = qpair->reserved_req;
	memcpy(&req->cmd, &cmd, sizeof(cmd));

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvmf_data->cntlid = 0xFFFF;
	} else {
		nvmf_data->cntlid = ctrlr->cntlid;
	}

	SPDK_STATIC_ASSERT(sizeof(nvmf_data->hostid) == sizeof(ctrlr->opts.extended_host_id),
			   "host ID size mismatch");
	memcpy(nvmf_data->hostid, ctrlr->opts.extended_host_id, sizeof(nvmf_data->hostid));
	snprintf(nvmf_data->hostnqn, sizeof(nvmf_data->hostnqn), "%s", ctrlr->opts.hostnqn);
	snprintf(nvmf_data->subnqn, sizeof(nvmf_data->subnqn), "%s", ctrlr->trid.subnqn);

	NVME_INIT_REQUEST(req, nvme_completion_poll_cb, status, NVME_PAYLOAD_CONTIG(nvmf_data, NULL),
			  sizeof(*nvmf_data), 0);

	rc = nvme_qpair_submit_request(qpair, req);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to allocate/submit FABRIC_CONNECT command, rc %d\n", rc);
		spdk_free(status->dma_data);
		free(status);
		return rc;
	}

	/* If we time out, the qpair will abort the request upon destruction. */
	if (ctrlr->opts.fabrics_connect_timeout_us > 0) {
		status->timeout_tsc = spdk_get_ticks() + ctrlr->opts.fabrics_connect_timeout_us *
				      spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	}

	qpair->poll_status = status;
	return 0;
}

int
nvme_fabric_qpair_connect_poll(struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status;
	struct spdk_nvmf_fabric_connect_rsp *rsp;
	struct spdk_nvme_ctrlr *ctrlr;
	int rc = 0;

	ctrlr = qpair->ctrlr;
	status = qpair->poll_status;

	if (nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL) == -EAGAIN) {
		return -EAGAIN;
	}

	if (status->timed_out || spdk_nvme_cpl_is_error(&status->cpl)) {
		SPDK_ERRLOG("Connect command failed, rc %d, trtype:%s adrfam:%s "
			    "traddr:%s trsvcid:%s subnqn:%s\n",
			    status->timed_out ? -ECANCELED : -EIO,
			    spdk_nvme_transport_id_trtype_str(ctrlr->trid.trtype),
			    spdk_nvme_transport_id_adrfam_str(ctrlr->trid.adrfam),
			    ctrlr->trid.traddr,
			    ctrlr->trid.trsvcid,
			    ctrlr->trid.subnqn);
		if (status->timed_out) {
			rc = -ECANCELED;
		} else {
			SPDK_ERRLOG("Connect command completed with error: sct %d, sc %d\n",
				    status->cpl.status.sct, status->cpl.status.sc);
			rc = -EIO;
		}

		goto finish;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status->cpl;
		ctrlr->cntlid = rsp->status_code_specific.success.cntlid;
		SPDK_DEBUGLOG(nvme, "CNTLID 0x%04" PRIx16 "\n", ctrlr->cntlid);
	}
finish:
	qpair->poll_status = NULL;
	if (!status->timed_out) {
		spdk_free(status->dma_data);
		free(status);
	}

	return rc;
}

int
nvme_fabric_qpair_connect(struct spdk_nvme_qpair *qpair, uint32_t num_entries)
{
	int rc;

	rc = nvme_fabric_qpair_connect_async(qpair, num_entries);
	if (rc) {
		return rc;
	}

	do {
		/* Wait until the command completes or times out */
		rc = nvme_fabric_qpair_connect_poll(qpair);
	} while (rc == -EAGAIN);

	return rc;
}
