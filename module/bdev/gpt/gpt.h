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

/** \file
 * GPT internal Interface
 */

#ifndef SPDK_INTERNAL_GPT_H
#define SPDK_INTERNAL_GPT_H

#include "spdk/stdinc.h"

#include "spdk/gpt_spec.h"

#define SPDK_GPT_PART_TYPE_GUID SPDK_GPT_GUID(0x7c5222bd, 0x8f5d, 0x4087, 0x9c00, 0xbf9843c7b58c)
#define SPDK_GPT_BUFFER_SIZE 32768  /* 32KB */
#define	SPDK_GPT_GUID_EQUAL(x,y) (memcmp(x, y, sizeof(struct spdk_gpt_guid)) == 0)

enum spdk_gpt_parse_phase {
	SPDK_GPT_PARSE_PHASE_INVALID = 0,
	SPDK_GPT_PARSE_PHASE_PRIMARY,
	SPDK_GPT_PARSE_PHASE_SECONDARY,
};

struct spdk_gpt {
	uint8_t parse_phase;
	unsigned char *buf;
	uint64_t buf_size;
	uint64_t lba_start;
	uint64_t lba_end;
	uint64_t total_sectors;
	uint32_t sector_size;
	struct spdk_gpt_header *header;
	struct spdk_gpt_partition_entry *partitions;
};

int gpt_parse_mbr(struct spdk_gpt *gpt);
int gpt_parse_partition_table(struct spdk_gpt *gpt);

#endif  /* SPDK_INTERNAL_GPT_H */
