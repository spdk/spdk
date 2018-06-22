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

/*
 * NVMe over Fabrics transport-independent functions
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/string.h"

static int
nvme_fabric_prop_set_cmd(struct spdk_nvme_ctrlr *ctrlr,
			 uint32_t offset, uint8_t size, uint64_t value)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	struct nvme_completion_poll_status status;
	int rc;

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
	cmd.ofst = offset;
	cmd.attrib.size = size;
	cmd.value.u64 = value;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					   NULL, 0,
					   nvme_completion_poll_cb, &status);
	if (rc < 0) {
		return rc;
	}

	if (spdk_nvme_wait_for_completion(ctrlr->adminq, &status)) {
		SPDK_ERRLOG("Property Set failed\n");
		return -1;
	}

	return 0;
}

static int
nvme_fabric_prop_get_cmd(struct spdk_nvme_ctrlr *ctrlr,
			 uint32_t offset, uint8_t size, uint64_t *value)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	struct nvme_completion_poll_status status;
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	int rc;

	assert(size == SPDK_NVMF_PROP_SIZE_4 || size == SPDK_NVMF_PROP_SIZE_8);

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	cmd.ofst = offset;
	cmd.attrib.size = size;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					   NULL, 0, nvme_completion_poll_cb,
					   &status);
	if (rc < 0) {
		return rc;
	}

	if (spdk_nvme_wait_for_completion(ctrlr->adminq, &status)) {
		SPDK_ERRLOG("Property Get failed\n");
		return -1;
	}

	response = (struct spdk_nvmf_fabric_prop_get_rsp *)&status.cpl;

	if (size == SPDK_NVMF_PROP_SIZE_4) {
		*value = response->value.u32.low;
	} else {
		*value = response->value.u64;
	}

	return 0;
}

int
nvme_fabric_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_fabric_prop_set_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value);
}

int
nvme_fabric_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_fabric_prop_set_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

int
nvme_fabric_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	uint64_t tmp_value;
	int rc;
	rc = nvme_fabric_prop_get_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, &tmp_value);

	if (!rc) {
		*value = (uint32_t)tmp_value;
	}
	return rc;
}

int
nvme_fabric_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_fabric_prop_get_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

static void
nvme_fabric_discover_probe(struct spdk_nvmf_discovery_log_page_entry *entry,
			   void *cb_ctx, spdk_nvme_probe_cb probe_cb)
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
	if (!spdk_nvme_transport_available(trid.trtype)) {
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
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Trailing newlines removed from discovery TRADDR\n");
	}

	/* Convert trsvcid to a null terminated string. */
	len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
	memcpy(trid.trsvcid, entry->trsvcid, len);
	if (spdk_str_chomp(trid.trsvcid) != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Trailing newlines removed from discovery TRSVCID\n");
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "subnqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n",
		      trid.subnqn, trid.trtype,
		      trid.traddr, trid.trsvcid);

	nvme_ctrlr_probe(&trid, NULL, probe_cb, cb_ctx);
}

static int
nvme_fabric_get_discovery_log_page(struct spdk_nvme_ctrlr *ctrlr,
				   void *log_page, uint32_t size, uint64_t offset)
{
	struct nvme_completion_poll_status status;
	int rc;

	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0, log_page, size, offset,
					      nvme_completion_poll_cb, &status);
	if (rc < 0) {
		return -1;
	}

	if (spdk_nvme_wait_for_completion(ctrlr->adminq, &status)) {
		return -1;
	}

	return 0;
}

int
nvme_fabric_ctrlr_discover(struct spdk_nvme_ctrlr *ctrlr,
			   void *cb_ctx, spdk_nvme_probe_cb probe_cb)
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
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Get Log Page - Discovery error\n");
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
			nvme_fabric_discover_probe(log_page_entry++, cb_ctx, probe_cb);
		}
		remaining_num_rec -= numrec;
		log_page_offset += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	} while (remaining_num_rec != 0);

	return 0;
}

int
nvme_fabric_qpair_connect(struct spdk_nvme_qpair *qpair, uint32_t num_entries)
{
	struct nvme_completion_poll_status status;
	struct spdk_nvmf_fabric_connect_rsp *rsp;
	struct spdk_nvmf_fabric_connect_cmd cmd;
	struct spdk_nvmf_fabric_connect_data *nvmf_data;
	struct spdk_nvme_ctrlr *ctrlr;
	int rc;

	if (num_entries == 0 || num_entries > SPDK_NVME_IO_QUEUE_MAX_ENTRIES) {
		return -EINVAL;
	}

	ctrlr = qpair->ctrlr;
	if (!ctrlr) {
		return -EINVAL;
	}

	nvmf_data = spdk_dma_zmalloc(sizeof(*nvmf_data), 0, NULL);
	if (!nvmf_data) {
		SPDK_ERRLOG("nvmf_data allocation error\n");
		return -ENOMEM;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.qid = qpair->id;
	cmd.sqsize = num_entries - 1;
	cmd.kato = ctrlr->opts.keep_alive_timeout_ms;

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

	rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair,
					(struct spdk_nvme_cmd *)&cmd,
					nvmf_data, sizeof(*nvmf_data),
					nvme_completion_poll_cb, &status);
	if (rc < 0) {
		SPDK_ERRLOG("Connect command failed\n");
		spdk_dma_free(nvmf_data);
		return rc;
	}

	if (spdk_nvme_wait_for_completion(qpair, &status)) {
		SPDK_ERRLOG("Connect command failed\n");
		spdk_dma_free(nvmf_data);
		return -EIO;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status.cpl;
		ctrlr->cntlid = rsp->status_code_specific.success.cntlid;
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "CNTLID 0x%04" PRIx16 "\n", ctrlr->cntlid);
	}

	spdk_dma_free(nvmf_data);
	return 0;
}
