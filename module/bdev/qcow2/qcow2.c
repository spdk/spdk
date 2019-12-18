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

#include "qcow2.h"

#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"

static struct spdk_qcow_header *
spdk_qcow2_get_header_buf(struct spdk_qcow2 *qcow2)
{
	switch (qcow2->parse_phase) {
	case SPDK_QCOW2_PARSE_PHASE_QCOW_HEADER:
		return (struct spdk_qcow_header *)qcow2->buf;
	default:
		assert(false);
	}
	return NULL;
}

static int
spdk_qcow2_read_tables(struct spdk_qcow2 *qcow2)
{
	return 0;

#if 0
	uint32_t total_partition_size, num_partition_entries, partition_entry_size;
	uint64_t partition_start_lba;
	struct spdk_qcow_header *head = qcow2->header;
	uint32_t crc32;

	num_partition_entries = from_le32(&head->num_partition_entries);
	if (num_partition_entries > SPDK_MAX_NUM_PARTITION_ENTRIES) {
		SPDK_ERRLOG("Num_partition_entries=%u which exceeds max=%u\n",
			    num_partition_entries, SPDK_MAX_NUM_PARTITION_ENTRIES);
		return -1;
	}

	partition_entry_size = from_le32(&head->size_of_partition_entry);
	if (partition_entry_size != sizeof(struct spdk_qcow2_partition_entry)) {
		SPDK_ERRLOG("Partition_entry_size(%x) != expected(%lx)\n",
			    partition_entry_size, sizeof(struct spdk_qcow2_partition_entry));
		return -1;
	}

	total_partition_size = num_partition_entries * partition_entry_size;
	partition_start_lba = from_le64(&head->partition_entry_lba);
	qcow2->partitions = spdk_qcow2_get_partitions_buf(qcow2, total_partition_size,
			    partition_start_lba);
	if (!qcow2->partitions) {
		SPDK_ERRLOG("Failed to get qcow2 partitions buf\n");
		return -1;
	}

	crc32 = spdk_crc32_ieee_update(qcow2->partitions, total_partition_size, ~0);
	crc32 ^= ~0;

	if (crc32 != from_le32(&head->partition_entry_array_crc32)) {
		SPDK_ERRLOG("GPT partition entry array crc32 did not match\n");
		return -1;
	}

	return 0;
#endif
}

int
spdk_qcow2_parse_mapping_table(struct spdk_qcow2 *qcow2)
{
	return 0;
}

static void
spdk_qcow2_dump_header_info(struct spdk_qcow_header *head)
{
	assert(head != NULL);

	printf("Dump the QCOW2 header info:\n");
	printf("\t maic:\t %x\n", head->magic);
	printf("\t version:\t %x\n", head->version);
	printf("\t backing_file_offset:\t %" PRIu64 "\n", head->backing_file_offset);
	printf("\t backing_file_size:\t %x\n", head->backing_file_size);
	printf("\t cluster_bits:\t %x\n", head->cluster_bits);
	printf("\t size:\t %" PRIu64 "\n", head->size);
	printf("\t crypt_method:\t %x\n", head->crypt_method);
	printf("\t l1_size:\t %x\n", head->l1_size);
	printf("\t l1_table_offset:\t %" PRIu64 "\n", head->l1_table_offset);
	printf("\t refcount_table_offset:\t %" PRIu64 "\n", head->refcount_table_offset);
	printf("\t refcount_table_clusters:\t %x\n", head->refcount_table_clusters);
	printf("\t nb_snapshots:\t %x\n", head->nb_snapshots);
	printf("\t snapshots_offset:\t %" PRIu64 "\n", head->snapshots_offset);
}

static void spdk_qcow2_header_convert(struct spdk_qcow_header *dst_head, struct spdk_qcow_header *org_head,
		bool from_disk_to_memory)
{
	if(from_disk_to_memory) {
		dst_head->magic = from_le32(&org_head->magic);
		dst_head->version = from_le32(&org_head->version);
		dst_head->backing_file_offset = from_le64(&org_head->backing_file_offset);
		dst_head->backing_file_size = from_le32(&org_head->backing_file_size);
		dst_head->cluster_bits = from_le32(&org_head->cluster_bits);
		dst_head->size = from_le64(&org_head->size);
		dst_head->crypt_method = from_le64(&org_head->crypt_method);
		dst_head->l1_size = from_le32(&org_head->l1_size);
		dst_head->l1_table_offset = from_le64(&org_head->l1_table_offset);
		dst_head->refcount_table_offset = from_le64(&org_head->refcount_table_offset);
		dst_head->refcount_table_clusters = from_le32(&org_head->refcount_table_clusters);
		dst_head->nb_snapshots = from_le32(&org_head->nb_snapshots);
		dst_head->snapshots_offset = from_le32(&org_head->snapshots_offset);
	} else {
		to_le32(&dst_head->magic, org_head->magic);
		to_le32(&dst_head->version, org_head->version);
		to_le64(&dst_head->backing_file_offset, org_head->backing_file_offset);
		to_le32(&dst_head->cluster_bits, org_head->cluster_bits);
		to_le32(&dst_head->size, org_head->size);
		to_le32(&dst_head->crypt_method, org_head->crypt_method);
		to_le32(&dst_head->l1_size, org_head->l1_size);
		to_le64(&dst_head->l1_table_offset, org_head->l1_table_offset);
		to_le64(&dst_head->refcount_table_offset, org_head->refcount_table_offset);
		to_le32(&dst_head->nb_snapshots, org_head->nb_snapshots);
		to_le32(&dst_head->snapshots_offset, org_head->snapshots_offset);
	}
}

static int
spdk_qcow2_read_header(struct spdk_qcow2 *qcow2)
{
	struct spdk_qcow_header *read_head;

	read_head = spdk_qcow2_get_header_buf(qcow2);
	if (!read_head) {
		SPDK_ERRLOG("Failed to get qcow2 header buf\n");
		return -1;
	}

	spdk_qcow2_header_convert(&qcow2->header, read_head, true);
	spdk_qcow2_dump_header_info(&qcow2->header);
	return 0;
}

int
spdk_qcow2_parse_header(struct spdk_qcow2 *qcow2)
{
	int rc;

	rc = spdk_qcow2_read_header(qcow2);
	if (rc) {
		SPDK_ERRLOG("Failed to read qcow2 header\n");
		return rc;
	}

	rc = spdk_qcow2_read_tables(qcow2);
	if (rc) {
		SPDK_ERRLOG("Failed to read qcow2 partitions\n");
		return rc;
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("qcow2_parse", SPDK_LOG_QCOW2_PARSE)
