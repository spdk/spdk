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

#include "gpt.h"

#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/event.h"

#include "spdk/log.h"

#define GPT_PRIMARY_PARTITION_TABLE_LBA 0x1
#define PRIMARY_PARTITION_NUMBER 4
#define GPT_PROTECTIVE_MBR 1
#define SPDK_MAX_NUM_PARTITION_ENTRIES 128

static uint64_t
gpt_get_expected_head_lba(struct spdk_gpt *gpt)
{
	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		return GPT_PRIMARY_PARTITION_TABLE_LBA;
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		return gpt->lba_end;
	default:
		assert(false);
	}
	return 0;
}

static struct spdk_gpt_header *
gpt_get_header_buf(struct spdk_gpt *gpt)
{
	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		return (struct spdk_gpt_header *)
		       (gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		return (struct spdk_gpt_header *)
		       (gpt->buf + (gpt->buf_size - gpt->sector_size));
	default:
		assert(false);
	}
	return NULL;
}

static struct spdk_gpt_partition_entry *
gpt_get_partitions_buf(struct spdk_gpt *gpt, uint64_t total_partition_size,
		       uint64_t partition_start_lba)
{
	uint64_t secondary_total_size;

	switch (gpt->parse_phase) {
	case SPDK_GPT_PARSE_PHASE_PRIMARY:
		if ((total_partition_size + partition_start_lba * gpt->sector_size) >
		    gpt->buf_size) {
			SPDK_ERRLOG("Buffer size is not enough\n");
			return NULL;
		}
		return (struct spdk_gpt_partition_entry *)
		       (gpt->buf + partition_start_lba * gpt->sector_size);
	case SPDK_GPT_PARSE_PHASE_SECONDARY:
		secondary_total_size = (gpt->lba_end - partition_start_lba + 1) * gpt->sector_size;
		if (secondary_total_size > gpt->buf_size) {
			SPDK_ERRLOG("Buffer size is not enough\n");
			return NULL;
		}
		return (struct spdk_gpt_partition_entry *)
		       (gpt->buf + (gpt->buf_size - secondary_total_size));
	default:
		assert(false);
	}
	return NULL;
}

static int
gpt_read_partitions(struct spdk_gpt *gpt)
{
	uint32_t total_partition_size, num_partition_entries, partition_entry_size;
	uint64_t partition_start_lba;
	struct spdk_gpt_header *head = gpt->header;
	uint32_t crc32;

	num_partition_entries = from_le32(&head->num_partition_entries);
	if (num_partition_entries > SPDK_MAX_NUM_PARTITION_ENTRIES) {
		SPDK_ERRLOG("Num_partition_entries=%u which exceeds max=%u\n",
			    num_partition_entries, SPDK_MAX_NUM_PARTITION_ENTRIES);
		return -1;
	}

	partition_entry_size = from_le32(&head->size_of_partition_entry);
	if (partition_entry_size != sizeof(struct spdk_gpt_partition_entry)) {
		SPDK_ERRLOG("Partition_entry_size(%x) != expected(%zx)\n",
			    partition_entry_size, sizeof(struct spdk_gpt_partition_entry));
		return -1;
	}

	total_partition_size = num_partition_entries * partition_entry_size;
	partition_start_lba = from_le64(&head->partition_entry_lba);
	gpt->partitions = gpt_get_partitions_buf(gpt, total_partition_size,
			  partition_start_lba);
	if (!gpt->partitions) {
		SPDK_ERRLOG("Failed to get gpt partitions buf\n");
		return -1;
	}

	crc32 = spdk_crc32_ieee_update(gpt->partitions, total_partition_size, ~0);
	crc32 ^= ~0;

	if (crc32 != from_le32(&head->partition_entry_array_crc32)) {
		SPDK_ERRLOG("GPT partition entry array crc32 did not match\n");
		return -1;
	}

	return 0;
}

static int
gpt_lba_range_check(struct spdk_gpt_header *head, uint64_t lba_end)
{
	uint64_t usable_lba_start, usable_lba_end;

	usable_lba_start = from_le64(&head->first_usable_lba);
	usable_lba_end = from_le64(&head->last_usable_lba);

	if (usable_lba_end < usable_lba_start) {
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") < usable_lba_start(%" PRIu64 ")\n",
			    usable_lba_end, usable_lba_start);
		return -1;
	}

	if (usable_lba_end > lba_end) {
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") > lba_end(%" PRIu64 ")\n",
			    usable_lba_end, lba_end);
		return -1;
	}

	if ((usable_lba_start < GPT_PRIMARY_PARTITION_TABLE_LBA) &&
	    (GPT_PRIMARY_PARTITION_TABLE_LBA < usable_lba_end)) {
		SPDK_ERRLOG("Head lba is not in the usable range\n");
		return -1;
	}

	return 0;
}

static int
gpt_read_header(struct spdk_gpt *gpt)
{
	uint32_t head_size;
	uint32_t new_crc, original_crc;
	uint64_t my_lba, head_lba;
	struct spdk_gpt_header *head;

	head = gpt_get_header_buf(gpt);
	if (!head) {
		SPDK_ERRLOG("Failed to get gpt header buf\n");
		return -1;
	}

	head_size = from_le32(&head->header_size);
	if (head_size < sizeof(*head) || head_size > gpt->sector_size) {
		SPDK_ERRLOG("head_size=%u\n", head_size);
		return -1;
	}

	original_crc = from_le32(&head->header_crc32);
	head->header_crc32 = 0;
	new_crc = spdk_crc32_ieee_update(head, from_le32(&head->header_size), ~0);
	new_crc ^= ~0;
	/* restore header crc32 */
	to_le32(&head->header_crc32, original_crc);

	if (new_crc != original_crc) {
		SPDK_ERRLOG("head crc32 does not match, provided=%u, caculated=%u\n",
			    original_crc, new_crc);
		return -1;
	}

	if (memcmp(SPDK_GPT_SIGNATURE, head->gpt_signature,
		   sizeof(head->gpt_signature))) {
		SPDK_ERRLOG("signature did not match\n");
		return -1;
	}

	head_lba = gpt_get_expected_head_lba(gpt);
	my_lba = from_le64(&head->my_lba);
	if (my_lba != head_lba) {
		SPDK_ERRLOG("head my_lba(%" PRIu64 ") != expected(%" PRIu64 ")\n",
			    my_lba, head_lba);
		return -1;
	}

	if (gpt_lba_range_check(head, gpt->lba_end)) {
		SPDK_ERRLOG("lba range check error\n");
		return -1;
	}

	gpt->header = head;
	return 0;
}

static int
gpt_check_mbr(struct spdk_gpt *gpt)
{
	int i, primary_partition = 0;
	uint32_t total_lba_size = 0, ret = 0, expected_start_lba;
	struct spdk_mbr *mbr;

	mbr = (struct spdk_mbr *)gpt->buf;
	if (from_le16(&mbr->mbr_signature) != SPDK_MBR_SIGNATURE) {
		SPDK_DEBUGLOG(gpt_parse, "Signature mismatch, provided=%x,"
			      "expected=%x\n", from_le16(&mbr->disk_signature),
			      SPDK_MBR_SIGNATURE);
		return -1;
	}

	for (i = 0; i < PRIMARY_PARTITION_NUMBER; i++) {
		if (mbr->partitions[i].os_type == SPDK_MBR_OS_TYPE_GPT_PROTECTIVE) {
			primary_partition = i;
			ret = GPT_PROTECTIVE_MBR;
			break;
		}
	}

	if (ret == GPT_PROTECTIVE_MBR) {
		expected_start_lba = GPT_PRIMARY_PARTITION_TABLE_LBA;
		if (from_le32(&mbr->partitions[primary_partition].start_lba) != expected_start_lba) {
			SPDK_DEBUGLOG(gpt_parse, "start lba mismatch, provided=%u, expected=%u\n",
				      from_le32(&mbr->partitions[primary_partition].start_lba),
				      expected_start_lba);
			return -1;
		}

		total_lba_size = from_le32(&mbr->partitions[primary_partition].size_lba);
		if ((total_lba_size != ((uint32_t) gpt->total_sectors - 1)) &&
		    (total_lba_size != 0xFFFFFFFF)) {
			SPDK_DEBUGLOG(gpt_parse,
				      "GPT Primary MBR size does not equal: (record_size %u != actual_size %u)!\n",
				      total_lba_size, (uint32_t) gpt->total_sectors - 1);
			return -1;
		}
	} else {
		SPDK_DEBUGLOG(gpt_parse, "Currently only support GPT Protective MBR format\n");
		return -1;
	}

	return 0;
}

int
gpt_parse_mbr(struct spdk_gpt *gpt)
{
	int rc;

	if (!gpt || !gpt->buf) {
		SPDK_ERRLOG("Gpt and the related buffer should not be NULL\n");
		return -1;
	}

	rc = gpt_check_mbr(gpt);
	if (rc) {
		SPDK_DEBUGLOG(gpt_parse, "Failed to detect gpt in MBR\n");
		return rc;
	}

	return 0;
}

int
gpt_parse_partition_table(struct spdk_gpt *gpt)
{
	int rc;

	rc = gpt_read_header(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt header\n");
		return rc;
	}

	rc = gpt_read_partitions(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt partitions\n");
		return rc;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(gpt_parse)
