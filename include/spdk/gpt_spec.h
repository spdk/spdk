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

/**
 * \file
 * GUID Partition Table (GPT) specification definitions
 */

#ifndef SPDK_GPT_SPEC_H
#define SPDK_GPT_SPEC_H

#include "spdk/stdinc.h"

#include "spdk/assert.h"

#pragma pack(push, 1)

#define SPDK_MBR_SIGNATURE 0xAA55

#define SPDK_MBR_OS_TYPE_GPT_PROTECTIVE		0xEE
#define SPDK_MBR_OS_TYPE_EFI_SYSTEM_PARTITION	0xEF

struct spdk_mbr_chs {
	uint8_t head;
	uint16_t sector : 6;
	uint16_t cylinder : 10;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr_chs) == 3, "size incorrect");

struct spdk_mbr_partition_entry {
	uint8_t reserved : 7;
	uint8_t bootable : 1;

	struct spdk_mbr_chs start_chs;

	uint8_t os_type;

	struct spdk_mbr_chs end_chs;

	uint32_t start_lba;
	uint32_t size_lba;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr_partition_entry) == 16, "size incorrect");

struct spdk_mbr {
	uint8_t boot_code[440];
	uint32_t disk_signature;
	uint16_t reserved_444;
	struct spdk_mbr_partition_entry partitions[4];
	uint16_t mbr_signature;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_mbr) == 512, "size incorrect");

#define SPDK_GPT_SIGNATURE "EFI PART"

#define SPDK_GPT_REVISION_1_0 0x00010000u

struct spdk_gpt_guid {
	uint8_t raw[16];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_guid) == 16, "size incorrect");

#define SPDK_GPT_GUID(a, b, c, d, e) \
	(struct spdk_gpt_guid){{ \
		(uint8_t)(a), (uint8_t)(((uint32_t)a) >> 8), \
		(uint8_t)(((uint32_t)a) >> 16), (uint8_t)(((uint32_t)a >> 24)), \
		(uint8_t)(b), (uint8_t)(((uint16_t)b) >> 8), \
		(uint8_t)(c), (uint8_t)(((uint16_t)c) >> 8), \
		(uint8_t)(((uint16_t)d) >> 8), (uint8_t)(d), \
		(uint8_t)(((uint64_t)e) >> 40), (uint8_t)(((uint64_t)e) >> 32), (uint8_t)(((uint64_t)e) >> 24), \
		(uint8_t)(((uint64_t)e) >> 16), (uint8_t)(((uint64_t)e) >> 8), (uint8_t)(e) \
	}}

#define SPDK_GPT_PART_TYPE_UNUSED		SPDK_GPT_GUID(0x00000000, 0x0000, 0x0000, 0x0000, 0x000000000000)
#define SPDK_GPT_PART_TYPE_EFI_SYSTEM_PARTITION	SPDK_GPT_GUID(0xC12A7328, 0xF81F, 0x11D2, 0xBA4B, 0x00A0C93EC93B)
#define SPDK_GPT_PART_TYPE_LEGACY_MBR		SPDK_GPT_GUID(0x024DEE41, 0x33E7, 0x11D3, 0x9D69, 0x0008C781F39F)

struct spdk_gpt_header {
	char gpt_signature[8];
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	struct spdk_gpt_guid disk_guid;
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t size_of_partition_entry;
	uint32_t partition_entry_array_crc32;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_header) == 92, "size incorrect");

struct spdk_gpt_partition_entry {
	struct spdk_gpt_guid part_type_guid;
	struct spdk_gpt_guid unique_partition_guid;
	uint64_t starting_lba;
	uint64_t ending_lba;
	struct {
		uint64_t required : 1;
		uint64_t no_block_io_proto : 1;
		uint64_t legacy_bios_bootable : 1;
		uint64_t reserved_uefi : 45;
		uint64_t guid_specific : 16;
	} attr;
	uint16_t partition_name[36];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_gpt_partition_entry) == 128, "size incorrect");

#pragma pack(pop)

#endif
