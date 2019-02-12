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
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"
#include "spdk/env.h"
#include "spdk/util.h"

static int outstanding_commands;

static int
zone_action_to_state(enum spdk_nvme_zone_action action)
{
	switch (action) {
	case SPDK_NVME_ZONE_ACTION_CLOSE:
		return SPDK_NVME_ZONE_STATE_CLOSED;
	case SPDK_NVME_ZONE_ACTION_FINISH:
		return SPDK_NVME_ZONE_STATE_FULL;
	case SPDK_NVME_ZONE_ACTION_OPEN:
		return SPDK_NVME_ZONE_STATE_EXPLICIT_OPEN;
	case SPDK_NVME_ZONE_ACTION_RESET:
		return SPDK_NVME_ZONE_STATE_EMPTY;
	default:
		assert(0);
		return SPDK_NVME_ZONE_STATE_OFFLINE;
	}
}

static void
command_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (cb_arg) {
		*(struct spdk_nvme_cpl *)cb_arg = *cpl;
	}

	outstanding_commands--;
}

static int
get_zone_info_log_page(struct spdk_nvme_ns *ns, struct spdk_nvme_zone_information_entry *entry,
		       uint64_t slba, size_t num_entries)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ns_data *cdata = spdk_nvme_ns_get_data(ns);
	struct spdk_nvme_cpl cpl = {};
	int nsid = spdk_nvme_ns_get_id(ns);
	uint64_t offset = slba / cdata->zsze;

	outstanding_commands = 0;

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_ZONE_INFORMATION,
					     nsid, entry, sizeof(*entry) * num_entries,
					     offset * sizeof(*entry),
					     command_completion, &cpl) == 0) {
		outstanding_commands++;
	} else {
		printf("get_zone_info_log_page failed\n");
		return -1;
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	return spdk_nvme_cpl_is_error(&cpl);
}

static int
change_zone_state(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		  uint64_t slba, enum spdk_nvme_zone_action action, struct spdk_nvme_cpl *ucpl)
{
	struct spdk_nvme_cpl cpl;
	outstanding_commands = 0;

	if (spdk_nvme_ns_cmd_zone_management(ns, qpair, slba, action,
					     command_completion, &cpl) == 0) {
		outstanding_commands++;
	} else {
		printf("spdk_nvme_ns_cmd_zone_management failed\n");
		return -1;
	}

	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (ucpl) {
		*ucpl = cpl;
	}

	return spdk_nvme_cpl_is_error(&cpl);
}

static int
change_state_and_check(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       uint64_t slba, enum spdk_nvme_zone_action action)
{
	struct spdk_nvme_zone_information_entry zone_entry;

	if (change_zone_state(ns, qpair, slba, action, NULL)) {
		printf("failed to open zone\n");
		return -1;
	}

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		return -1;
	}

	if (zone_entry.zs != zone_action_to_state(action)) {
		printf("unexpected zone state\n");
		return -1;
	}

	return 0;
}

static int
test_num_used_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	struct spdk_nvme_cpl cpl = {};
	uint32_t num_used;
	uint64_t lba = 0;

	for (num_used = 0; num_used < cdata->nar; ++num_used) {
		if (change_state_and_check(ns, qpair, lba, SPDK_NVME_ZONE_ACTION_OPEN)) {
			return -1;
		}
		if (change_state_and_check(ns, qpair, lba, SPDK_NVME_ZONE_ACTION_CLOSE)) {
			return -1;
		}

		lba += nsdata->zsze;
	}

	/* check that it's not possible to open another zone after reaching nar */
	if (change_zone_state(ns, qpair, lba, SPDK_NVME_ZONE_ACTION_OPEN, &cpl)) {
		if (cpl.status.sct != SPDK_NVME_SCT_GENERIC ||
		    cpl.status.sc  != SPDK_NVME_SC_ZONE_TOO_MANY_ACTIVE) {
			printf("unexpected status code\n");
			return -1;
		}
	} else {
		printf("successfully opened a zone exceeding NOR limit\n");
		return -1;
	}

	for (num_used = 0; num_used < cdata->nar; ++num_used) {
		if (change_state_and_check(ns, qpair, num_used * nsdata->zsze,
					   SPDK_NVME_ZONE_ACTION_RESET)) {
			return -1;
		}
	}

	lba = 0;
	for (num_used = 0; num_used < cdata->nar; ++num_used) {
		if (change_state_and_check(ns, qpair, lba, SPDK_NVME_ZONE_ACTION_OPEN)) {
			return -1;
		}

		lba += nsdata->zsze;
	}
	/* check that it's not possible to open another zone after reaching nar */
	if (change_zone_state(ns, qpair, lba, SPDK_NVME_ZONE_ACTION_OPEN, &cpl)) {
		if (cpl.status.sct != SPDK_NVME_SCT_GENERIC ||
		    cpl.status.sc  != SPDK_NVME_SC_ZONE_TOO_MANY_OPEN) {
			printf("unexpected status code\n");
			return -1;
		}
	} else {
		printf("successfully opened a zone exceeding NOR limit\n");
		return -1;
	}

	for (num_used = 0; num_used < cdata->nar; ++num_used) {
		if (change_state_and_check(ns, qpair, num_used * nsdata->zsze,
					   SPDK_NVME_ZONE_ACTION_RESET)) {
			return -1;
		}
	}

	return 0;
}

static int
test_valid_state_transitions(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	uint64_t slba = nsdata->zsze;

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		return -1;
	}

	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_EMPTY) {
		printf("unexpected zone state\n");
		return -1;
	}

	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_CLOSE)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_FINISH)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		return -1;
	}

	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_CLOSE)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_FINISH)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		return -1;
	}

	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		return -1;
	}

	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_CLOSE)) {
		return -1;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		return -1;
	}

	return 0;
}

static int
write_verify_write_pointer(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			   uint64_t slba, uint32_t num_lbas, struct spdk_nvme_cpl *ucpl)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	struct spdk_nvme_cpl *cpl, icpl;
	uint64_t write_pointer;

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		return -1;
	}

	cpl = ucpl ? ucpl : &icpl;
	write_pointer = zone_entry.wp;
	if (spdk_nvme_ns_cmd_write(ns, qpair, payload, slba, num_lbas,
				   command_completion, cpl, 0) == 0) {
		outstanding_commands++;
	} else {
		printf("spdk_nvme_ns_cmd_write failed\n");
		return -1;
	}

	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("spdk_nvme_ns_cmd_write returned non-zero status\n");
		return -1;
	}

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		return -1;
	}

	if (write_pointer + num_lbas != zone_entry.wp) {
		printf("unexpected write pointer value: (%"PRIu64" != %"PRIu64")\n",
		       write_pointer + num_lbas, zone_entry.wp);
		return -1;
	}

	return 0;
}

static int
read_data(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	  void *payload, uint64_t slba, uint32_t num_lbas)
{
	struct spdk_nvme_cpl cpl;

	if (spdk_nvme_ns_cmd_read(ns, qpair, payload, slba, num_lbas,
				  command_completion, &cpl, 0) == 0) {
		outstanding_commands++;
	} else {
		printf("spdk_nvme_ns_cmd_read failed\n");
		return -1;
	}

	while (outstanding_commands) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	if (spdk_nvme_cpl_is_error(&cpl)) {
		printf("spdk_nvme_ns_cmd_write returned non-zero status\n");
		return -1;
	}

	return 0;
}

static int
test_io_states(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	uint64_t slba = nsdata->zsze, sector, capacity;
	struct spdk_nvme_cpl cpl;
	void *buffer;
	int rc = -1;

	buffer = spdk_dma_zmalloc(2 * spdk_nvme_ns_get_extended_sector_size(ns), 0, NULL);
	if (!buffer) {
		printf("failed to allocate write data buffer\n");
		goto out;
	}

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}

	capacity = zone_entry.zcap;
	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_EMPTY || zone_entry.wp != zone_entry.zslba) {
		printf("unexpected zone state\n");
		goto out;
	}

	/* Verify that reset sets write pointer = 0 */
	if (write_verify_write_pointer(ns, qpair, buffer, slba, 1, NULL)) {
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}
	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.wp != zone_entry.zslba) {
		printf("unexpected write pointer value\n");
		goto out;
	}

	/* Verify that closing a zone doesn't change its write pointer */
	if (write_verify_write_pointer(ns, qpair, buffer, slba, 1, NULL)) {
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_CLOSE)) {
		goto out;
	}
	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.wp != zone_entry.zslba + 1) {
		printf("unexpected write pointer value\n");
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_OPEN)) {
		goto out;
	}
	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.wp != zone_entry.zslba + 1) {
		printf("unexpected write pointer value\n");
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}

	/* Verify that a zone is set to full once all of its blocks are filled */
	for (sector = 0; sector < capacity; ++sector) {
		if (write_verify_write_pointer(ns, qpair, buffer, slba + sector, 1, NULL)) {
			goto out;
		}
	}
	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_FULL) {
		printf("unexpeceted zone state\n");
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}

	/*
	 * Verify that a zone is set to full and early finish is returned, when more then possible
	 * number of bytes are requested to be written
	 */
	for (sector = 0; sector < capacity - 1; ++sector) {
		if (write_verify_write_pointer(ns, qpair, buffer, slba + sector, 1, NULL)) {
			goto out;
		}
	}
	if (!write_verify_write_pointer(ns, qpair, buffer, slba + sector, 2, &cpl)) {
		printf("unexpected successful write completion\n");
		goto out;
	}
	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_FULL) {
		printf("unexpeceted zone state\n");
		goto out;
	}
	if (zone_entry.wp != zone_entry.zslba + capacity - 1) {
		printf("unexpected write pointer value\n");
		goto out;
	}
	if (cpl.status.sct != SPDK_NVME_SCT_GENERIC ||
	    cpl.status.sc  != SPDK_NVME_SC_ZONE_EARLY_FINISH) {
		printf("unexpected status code\n");
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}

	rc = 0;
out:
	return rc;
}

static int
test_basic_integrity(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	uint64_t slba = nsdata->zsze, sector_size, capacity;
#define TRANSFER_SIZE 16
#define TRANSFER_COUNT 4
	void *buffer[TRANSFER_COUNT] = {}, *rbuffer;
	unsigned int transfer, i;
	int rc = -1;

	sector_size = spdk_nvme_ns_get_extended_sector_size(ns);

	rbuffer = spdk_dma_zmalloc(sector_size * TRANSFER_SIZE * (TRANSFER_COUNT + 1), 0, NULL);
	if (!rbuffer) {
		printf("failed to allocate data buffer\n");
		goto out;
	}

	for (transfer = 0; transfer < TRANSFER_COUNT; ++transfer) {
		buffer[transfer] = (char *)rbuffer + TRANSFER_SIZE * sector_size * (transfer + 1);
		for (i = 0; i < TRANSFER_SIZE * sector_size / sizeof(int); ++i) {
			*((int *)(buffer[transfer]) + i) = rand();
		}
	}

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}

	capacity = zone_entry.zcap;
	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_EMPTY || zone_entry.wp != zone_entry.zslba) {
		printf("unexpected zone state\n");
		goto out;
	}
	if (capacity < TRANSFER_COUNT * TRANSFER_SIZE) {
		printf("test parameters exceed zone size\n");
		goto out;
	}

	for (transfer = 0; transfer < TRANSFER_COUNT; ++transfer) {
		if (write_verify_write_pointer(ns, qpair, buffer[transfer],
					       slba + transfer * TRANSFER_SIZE,
					       TRANSFER_SIZE, NULL)) {
			goto out;
		}
	}

	for (transfer = 0; transfer < TRANSFER_COUNT; ++transfer) {
		if (read_data(ns, qpair, rbuffer, slba + transfer * TRANSFER_SIZE,
			      TRANSFER_SIZE)) {
			goto out;
		}

		if (memcmp(rbuffer, buffer[transfer], TRANSFER_SIZE * sector_size)) {
			printf("data integrity verification failed (transfer: %u)\n", transfer);
			goto out;
		}
	}

	/* Change the state of the zone and verify the data is there */
	const int zone_actions[] = {
		SPDK_NVME_ZONE_ACTION_CLOSE,
		SPDK_NVME_ZONE_ACTION_OPEN,
		SPDK_NVME_ZONE_ACTION_FINISH,
	};
	for (i = 0; i < SPDK_COUNTOF(zone_actions); ++i) {
		if (change_state_and_check(ns, qpair, slba, zone_actions[i])) {
			goto out;
		}
		for (transfer = 0; transfer < TRANSFER_COUNT; ++transfer) {
			if (read_data(ns, qpair, rbuffer, slba + transfer * TRANSFER_SIZE,
				      TRANSFER_SIZE)) {
				goto out;
			}

			if (memcmp(rbuffer, buffer[transfer], TRANSFER_SIZE * sector_size)) {
				printf("data integrity verification failed (transfer: %u)\n",
				       transfer);
				goto out;
			}
		}
	}

	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}

	rc = 0;
out:
	spdk_dma_free(rbuffer);
	return rc;
}

struct io_context {
	struct spdk_nvme_cpl	cpl;
	uint64_t		lba;
	uint32_t		*num_outstanding;
	void			*wdata;
	void			*rdata;
};

static void
append_verify_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_context *io = cb_arg;

	io->cpl = *cpl;
	(*io->num_outstanding)--;
}

static void
append_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_context *io = cb_arg;

	io->cpl = *cpl;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		io->lba = ((uint64_t)cpl->cdw1 << 32) | cpl->cdw0;
	}

	(*io->num_outstanding)--;
}

static int
test_append(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
	    uint64_t slba, uint32_t num_lbas, uint32_t qdepth)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	uint32_t request, sector_size, num_outstanding = 0;
	struct io_context io[qdepth];
	void *rbuffer = NULL, *wbuffer;
	int rc = -1;

	memset(&io, 0, qdepth * sizeof(struct io_context));
	sector_size = spdk_nvme_ns_get_extended_sector_size(ns);

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}

	rbuffer = spdk_dma_zmalloc(sector_size * qdepth * 2, 0, NULL);
	if (!rbuffer) {
		printf("failed to allocate data buffer\n");
		goto out;
	}

	wbuffer = (char *)rbuffer + sector_size * qdepth;

#define LBA_INVALID ((uint64_t)-1)
	for (request = 0; request < qdepth; ++request) {
		io[request] = (struct io_context) {
			.num_outstanding = &num_outstanding,
			.wdata = (char *)wbuffer + request * sector_size,
			.rdata = (char *)rbuffer + request * sector_size,
		};

		uint32_t dword;
		for (dword = 0; dword < sector_size / sizeof(int); ++dword) {
			*(uint32_t *)io[request].wdata = rand();
		}
	}

	while (num_lbas) {
		uint32_t num_requests = spdk_min(num_lbas, qdepth);

		for (request = 0; request < num_requests; ++request) {
			struct io_context *current = &io[request];
			current->lba = LBA_INVALID;
			current->cpl = (struct spdk_nvme_cpl) {};

			if (spdk_nvme_ns_cmd_zone_append(ns, qpair, current->wdata, slba, 1,
							 append_completion, current, 0) == 0) {
				num_outstanding++;
			} else {
				printf("spdk_nvme_ns_cmd_zone_append failed\n");
				goto out;
			}
		}

		while (num_outstanding != 0) {
			spdk_nvme_qpair_process_completions(qpair, num_requests);
		}

		for (request = 0; request < num_requests; ++request) {
			struct io_context *current = &io[request];

			if (spdk_nvme_cpl_is_error(&current->cpl)) {
				printf("spdk_nvme_ns_cmd_zone_append returned an error status\n");
				goto out;
			}

			if (current->lba < slba || current->lba >= slba + zone_entry.zcap) {
				printf("append returned LBA out of current zone range\n");
				goto out;
			}

			if (spdk_nvme_ns_cmd_read(ns, qpair, current->rdata, current->lba,
						  1, append_verify_completion, current, 0) == 0) {
				num_outstanding++;
			} else {
				printf("spdk_nvme_ns_cmd_read failed\n");
				goto out;
			}
		}

		while (num_outstanding != 0) {
			spdk_nvme_qpair_process_completions(qpair, num_requests);
		}

		for (request = 0; request < num_requests; ++request) {
			struct io_context *current = &io[request];

			if (spdk_nvme_cpl_is_error(&current->cpl)) {
				printf("append failed @LBA:%"PRIu64"\n", current->lba);
				goto out;
			}

			if (memcmp(current->rdata, current->wdata, sector_size)) {
				printf("data integrity failed @LBA:%"PRIu64"\n", current->lba);
				goto out;
			}
		}

		num_lbas -= num_requests;
	}


	rc = 0;
out:
	while (outstanding_commands != 0) {
		spdk_nvme_qpair_process_completions(qpair, 1);
	}

	spdk_dma_free(rbuffer);
	return rc;
}

static int
test_basic_append(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_zone_information_entry zone_entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	uint64_t slba = nsdata->zsze;
	int rc = -1;

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}

	if (test_append(ns, qpair, slba, zone_entry.zcap, 64)) {
		goto out;
	}

	if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
		printf("get_zone_info_log_page failed\n");
		goto out;
	}
	if (zone_entry.zs != SPDK_NVME_ZONE_STATE_FULL) {
		printf("unexpceted zone state\n");
		goto out;
	}
	if (zone_entry.wp != zone_entry.zslba + zone_entry.zcap) {
		printf("unexpected write pointer value\n");
		goto out;
	}
	if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
		goto out;
	}

	rc = 0;
out:
	return rc;
}

struct worker_context {
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	pthread_t		tid;
	uint64_t		slba;
	uint32_t		num_lbas;
	uint32_t		qdepth;
	int			rc;
};

static void *
append_worker(void *ctx)
{
	struct worker_context *worker = ctx;

	worker->rc = test_append(worker->ns, worker->qpair, worker->slba,
				 worker->num_lbas, worker->qdepth);
	return NULL;
}

static int
test_mt_append(struct spdk_nvme_ns *ns)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);
	struct spdk_nvme_zone_information_entry zone_entry;
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
#define WORKER_COUNT 32
#define WORKERS_PER_ZONE 4
	struct worker_context *current, worker[WORKER_COUNT] = {};
	unsigned int wid;
	int rc = -1;

	for (wid = 0; wid < WORKER_COUNT; ++wid) {
		current = &worker[wid];
		current->ns = ns;
		current->qdepth = 32;
		current->slba = nsdata->zsze * (wid / WORKERS_PER_ZONE);

		if (get_zone_info_log_page(ns, &zone_entry, current->slba, 1)) {
			printf("get_zone_info_log_page failed\n");
			goto out;
		}
		if (zone_entry.zcap % WORKERS_PER_ZONE) {
			printf("invalid number of workers per zone\n");
			goto out;
		}

		current->num_lbas = zone_entry.zcap / WORKERS_PER_ZONE;
		current->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
		if (!current->qpair) {
			printf("spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto out;
		}
	}

	for (wid = 0; wid < WORKER_COUNT; ++wid) {
		current = &worker[wid];
		if (pthread_create(&current->tid, NULL, append_worker, current)) {
			printf("pthread_create failed\n");
			goto out;
		}
	}

	rc = 0;
	for (wid = 0; wid < WORKER_COUNT; ++wid) {
		current = &worker[wid];
		if (pthread_join(current->tid, NULL)) {
			printf("pthread_join failed\n");
			rc = -1;
			continue;
		}

		if (current->rc) {
			printf("worker #%u failed\n", wid);
			rc = -1;
		}
	}

	for (wid = 0; wid < WORKER_COUNT; wid += WORKERS_PER_ZONE) {
		current = &worker[wid];
		if (get_zone_info_log_page(ns, &zone_entry, current->slba, 1)) {
			printf("get_zone_info_log_page failed\n");
			rc = -1;
			continue;
		}
		if (zone_entry.zs != SPDK_NVME_ZONE_STATE_FULL) {
			printf("unexpected zone state\n");
			rc = -1;
		}
		if (zone_entry.wp != zone_entry.zslba + zone_entry.zcap) {
			printf("unexpected write pointer value\n");
			rc = -1;
		}
		if (change_state_and_check(ns, current->qpair, current->slba,
					   SPDK_NVME_ZONE_ACTION_RESET)) {
			rc = -1;
		}
	}
out:
	for (wid = 0; wid < WORKER_COUNT; ++wid) {
		spdk_nvme_ctrlr_free_io_qpair(worker[wid].qpair);
	}
	return rc;
}

static int
reset_zones(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair)
{
	const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns);
	struct spdk_nvme_zone_information_entry zone_entry;
	uint64_t num_zones = nsdata->nsze / nsdata->zsze, zoneid, slba;

	for (zoneid = 0; zoneid < num_zones; ++zoneid) {
		slba = zoneid * nsdata->zsze;
		if (get_zone_info_log_page(ns, &zone_entry, slba, 1)) {
			printf("get_zone_info_log_page failed\n");
			return -1;
		}

		if (zone_entry.zs == SPDK_NVME_ZONE_STATE_EMPTY) {
			continue;
		}

		if (change_state_and_check(ns, qpair, slba, SPDK_NVME_ZONE_ACTION_RESET)) {
			return -1;
		}
	}

	return 0;
}

static void
test_controller(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	uint32_t nsid;

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!qpair) {
		printf("spdk_nvme_ctrlr_alloc_io_qpair failed\n");
		return;
	}

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

		if (reset_zones(ns, qpair)) {
			printf("reset zones failed\n");
			break;
		}

		if (test_valid_state_transitions(ns, qpair)) {
			printf("test_valid_state_transitions failed\n");
			break;
		}

		if (test_num_used_zones(ns, qpair)) {
			printf("test_num_used_zones failed\n");
			break;
		}

		if (test_io_states(ns, qpair)) {
			printf("test_io_states failed\n");
			break;
		}

		if (test_basic_integrity(ns, qpair)) {
			printf("test_basic_integrity failed\n");
			break;
		}

		if (test_basic_append(ns, qpair)) {
			printf("test_basic_append failed\n");
			break;
		}

		if (test_mt_append(ns)) {
			printf("test_mt_append failed\n");
			break;
		}

		printf("%s[%"PRIu32"]: success\n", trid->traddr, nsid);
	}

	spdk_nvme_ctrlr_free_io_qpair(qpair);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	if (spdk_nvme_ctrlr_is_zns_supported(ctrlr)) {
		test_controller(ctrlr, trid);
	}
	spdk_nvme_detach(ctrlr);
}

int main(int argc, char **argv)
{
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);

	srand(time(NULL));

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
}
