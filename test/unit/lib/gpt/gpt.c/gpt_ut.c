/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2016 FUJITSU LIMITED, All rights reserved.
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
#undef SPDK_CONFIG_VTUNE

#include "gpt/gpt.c"


static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}


static void
spdk_gpt_parse_test(void)
{
	struct spdk_gpt *gpt;
	int i, re;
	unsigned char a[32770];

	gpt = calloc(1, sizeof(*gpt));

	unsigned char ch='a';
	for (i=0;i<32768;i++)
		a[i]=ch;

	gpt->buf = &a[0];
	gpt->buf[32768] = '\0';
	printf("strlen(gpt->buf) = %lu\n",strlen(gpt->buf));

	/* Set gpt is NULL */
	re = spdk_gpt_parse(NULL);
	CU_ASSERT(re == -1);

	/* Set gpt->buf is NULL */
	gpt->buf = NULL;
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	gpt->buf = &a[0];
	gpt->buf[32768] = '\0';
	/* Set gpt is "aaa..." */
	re = spdk_gpt_parse(gpt);
	CU_ASSERT(re == -1);

	/* Set mbr mbr_signature = 0xAA55; */
	/* Set mbr partitions[0].start_lba = 0x1 */
	struct spdk_mbr *mbr;
	mbr = (struct spdk_mbr *)gpt->buf;
	mbr->mbr_signature = 0xAA55;
	mbr->partitions[0].start_lba = 1;
	re = spdk_gpt_parse(gpt);
	mbr = NULL;
	CU_ASSERT(re == -1);

	/* Set mbr partitions[0].os_type = 0xEE */
	mbr = (struct spdk_mbr *)gpt->buf;
	mbr->partitions[0].os_type = 0xEE;
	re = spdk_gpt_parse(gpt);
	mbr = NULL;
	CU_ASSERT(re == -1);

	/* Set mbr partitions[0].size_lba = 0xFFFFFFFF */
	mbr = (struct spdk_mbr *)gpt->buf;
	mbr->partitions[0].size_lba = 0xFFFFFFFF;
	re = spdk_gpt_parse(gpt);
	mbr = NULL;
	CU_ASSERT(re == -1);

	/* Set head header_size = 600 */
	gpt->sector_size = 512;
	struct spdk_gpt_header *head;
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_size = 600;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set head header_crc32 = 0x22D18C80 */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_size = 92;
	head->header_crc32 = 0x22D18C80;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set head header_crc32 = 0xC5B2117E*/
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0xC5B2117E;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set head gpt_signature = "EFI PART" */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0xD637335A;
	head->gpt_signature[0] = 'E';
	head->gpt_signature[1] = 'F';
	head->gpt_signature[2] = 'I';
	head->gpt_signature[3] = ' ';
	head->gpt_signature[4] = 'P';
	head->gpt_signature[5] = 'A';
	head->gpt_signature[6] = 'R';
	head->gpt_signature[7] = 'T';
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set head gpt->lba_end = 781410302 */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0x30CB7378;   // why the value is changing?
	gpt->lba_start = 0;
	gpt->lba_end = 781410302;
	head->first_usable_lba = 10;
	head->last_usable_lba = 1000000;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set partition head->num_partition_entries = 64 */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0x573857BE;
	head->num_partition_entries = 64;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set partition head->size_of_partition_entry = 128 */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0x5279B712;
	head->size_of_partition_entry = 128;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set partition head->partition_entry_lba = 32 */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0xec093b43;
	head->num_partition_entries = 64;
	head->size_of_partition_entry = 128;
	head->partition_entry_lba = 32;
	gpt->sector_size = 512;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == -1);

	/* Set partition partition_entry_array_crc32 = 0xebee44fb */
	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head->header_crc32 = 0xe1a08822;
	head->partition_entry_array_crc32 = 0xebee44fb;
	head->num_partition_entries = 128;
	head->size_of_partition_entry = 128;
	re = spdk_gpt_parse(gpt);
	head  = NULL;
	CU_ASSERT(re == 0);

	printf("spdk_gpt_parse exit sucessfully(return 0).\n");

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

	suite = CU_add_suite("gpt_parse", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "gpt_parse_ut", spdk_gpt_parse_test) == NULL
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
