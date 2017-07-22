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
 *     * Neither the name of the copyright holder nor the names of its
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
#include "spdk_cunit.h"
#include "lib/test_env.c"
#include "gpt/gpt.c"

struct spdk_gpt *gpt;
struct spdk_mbr *mbr;
struct spdk_gpt_header *head;
unsigned char a[SPDK_GPT_BUFFER_SIZE];
int re;

static void
check_mbr_test(void)
{
	gpt = calloc(1, sizeof(*gpt));
	SPDK_CU_ASSERT_FATAL(gpt != NULL);

	/* Set *gpt is "aaa...", all are mismatch include mbr_signature */
	memset(a, 'a', sizeof(a));
	gpt->buf = &a[0];
	re = spdk_gpt_check_mbr(gpt);
	CU_ASSERT(re == -1);

	/* Set mbr->mbr_signature matched, start lba mismatch */
	mbr = (struct spdk_mbr *)gpt->buf;
	mbr->mbr_signature = 0xAA55;
	re = spdk_gpt_check_mbr(gpt);
	CU_ASSERT(re == -1);

	/* Set mbr->partitions[0].start lba matched, os_type mismatch */
	mbr->partitions[0].start_lba = 1;
	re = spdk_gpt_check_mbr(gpt);
	CU_ASSERT(re == -1);

	/* Set mbr->partitions[0].os_type matched, size_lba mismatch */
	mbr->partitions[0].os_type = 0xEE;
	re = spdk_gpt_check_mbr(gpt);
	CU_ASSERT(re == -1);

	/* Set mbr->partitions[0].size_lba matched */
	mbr->partitions[0].size_lba = 0xFFFFFFFF;
	re = spdk_gpt_check_mbr(gpt);
	CU_ASSERT(re == 0);

	free(gpt);
}

static void
read_header_test(void)
{
	gpt = calloc(1, sizeof(*gpt));
	SPDK_CU_ASSERT_FATAL(gpt != NULL);

	/* Set *gpt is "aaa...", all are mismatch include header_size */
	memset(a, 'a', sizeof(a));
	gpt->buf = &a[0];
	gpt->sector_size = 512;
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_size = 600;
	re = spdk_gpt_read_header(gpt);
	CU_ASSERT(re == -1);

	/* Set head->header_size matched, header_crc32 mismatch */
	head->header_size = 92;
	head->header_crc32 = 0x22D18C80;
	re = spdk_gpt_read_header(gpt);
	CU_ASSERT(re == -1);

	/* Set head->header_crc32 matched, gpt_signature mismatch */
	head->header_crc32 = 0xC5B2117E;
	re = spdk_gpt_read_header(gpt);
	CU_ASSERT(re == -1);

	/* Set head->gpt_signature matched, lba_end usable_lba mismatch */
	head->header_crc32 = 0xD637335A;
	head->gpt_signature[0] = 'E';
	head->gpt_signature[1] = 'F';
	head->gpt_signature[2] = 'I';
	head->gpt_signature[3] = ' ';
	head->gpt_signature[4] = 'P';
	head->gpt_signature[5] = 'A';
	head->gpt_signature[6] = 'R';
	head->gpt_signature[7] = 'T';
	re = spdk_gpt_read_header(gpt);
	CU_ASSERT(re == -1);

	/* Set gpt->lba_end usable_lba matched */
	head->header_crc32 = 0x30CB7378;
	gpt->lba_start = 0;
	gpt->lba_end = 781410302;
	head->first_usable_lba = 10;
	head->last_usable_lba = 1000000;
	re = spdk_gpt_read_header(gpt);
	gpt->header = head;
	CU_ASSERT(re == 0);

	free(gpt);
}

static void
read_partitions_test(void)
{
	gpt = calloc(1, sizeof(*gpt));
	SPDK_CU_ASSERT_FATAL(gpt != NULL);

	/* Set *gpt is "aaa...", all are mismatch include num_partition_entries */
	memset(a, 'a', sizeof(a));
	gpt->buf = &a[0];
	gpt->sector_size = 512;
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	gpt->header = head;
	head->num_partition_entries = 256;
	re = spdk_gpt_read_partitions(gpt);
	CU_ASSERT(re == -1);

	/* Set num_partition_entries matched, size_of_partition_entry mismatch */
	head->header_crc32 = 0x573857BE;
	head->num_partition_entries = 64;
	re = spdk_gpt_read_partitions(gpt);
	CU_ASSERT(re == -1);

	/* Set size_of_partition_entry matched, partition_entry_lba mismatch */
	head->header_crc32 = 0x5279B712;
	head->size_of_partition_entry = 128;
	re = spdk_gpt_read_partitions(gpt);
	CU_ASSERT(re == -1);

	/* Set partition_entry_lba matched, partition_entry_array_crc32 mismatch */
	head->header_crc32 = 0xec093b43;
	head->partition_entry_lba = 32;
	gpt->sector_size = 512;
	re = spdk_gpt_read_partitions(gpt);
	CU_ASSERT(re == -1);

	/* Set partition_entry_array_crc32 matched, all matched */
	head->header_crc32 = 0xe1a08822;
	head->partition_entry_array_crc32 = 0xebee44fb;
	head->num_partition_entries = 128;
	re = spdk_gpt_read_partitions(gpt);
	CU_ASSERT(re == 0);

	free(gpt);
}

static void
parse_test(void)
{
	/* Set gpt is NULL */
	re = spdk_gpt_parse(NULL);
	CU_ASSERT(re == -1);

	/* Set gpt->buf is NULL */
	gpt = calloc(1, sizeof(*gpt));
	SPDK_CU_ASSERT_FATAL(gpt != NULL);
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	/* Set *gpt is "aaa...", check_mbr failed */
	memset(a, 'a', sizeof(a));
	gpt->buf = &a[0];
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	/* Set check_mbr passed, read_header failed */
	mbr = (struct spdk_mbr *)gpt->buf;
	mbr->mbr_signature = 0xAA55;
	mbr->partitions[0].start_lba = 1;
	mbr->partitions[0].os_type = 0xEE;
	mbr->partitions[0].size_lba = 0xFFFFFFFF;
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	/* Set read_header passed, read_partitions failed */
	gpt->sector_size = 512;
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_size = 92;
	head->gpt_signature[0] = 'E';
	head->gpt_signature[1] = 'F';
	head->gpt_signature[2] = 'I';
	head->gpt_signature[3] = ' ';
	head->gpt_signature[4] = 'P';
	head->gpt_signature[5] = 'A';
	head->gpt_signature[6] = 'R';
	head->gpt_signature[7] = 'T';
	head->header_crc32 = 0x30CB7378;
	gpt->lba_start = 0;
	gpt->lba_end = 781410302;
	head->first_usable_lba = 10;
	head->last_usable_lba = 1000000;
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	/* Set read_partitions passed, all passed */
	head->size_of_partition_entry = 128;
	head->partition_entry_lba = 32;
	head->header_crc32 = 0xe1a08822;
	head->partition_entry_array_crc32 = 0xebee44fb;
	head->num_partition_entries = 128;
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == 0);

	free(gpt);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("gpt_parse", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "parse", parse_test) == NULL ||
		CU_add_test(suite, "check mbr", check_mbr_test) == NULL ||
		CU_add_test(suite, "read header", read_header_test) == NULL ||
		CU_add_test(suite, "read partitions", read_partitions_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
