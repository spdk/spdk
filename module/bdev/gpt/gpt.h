/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * GPT internal Interface
 */

#ifndef SPDK_INTERNAL_GPT_H
#define SPDK_INTERNAL_GPT_H

#include "spdk/stdinc.h"

#include "spdk/gpt_spec.h"
#include "spdk/log.h"

#define SPDK_GPT_PART_TYPE_GUID     SPDK_GPT_GUID(0x6527994e, 0x2c5a, 0x4eec, 0x9613, 0x8f5944074e8b)

/* PART_TYPE_GUID_OLD partitions will be constructed as bdevs with one fewer block than expected.
 * See GitHub issue #2801.
 */
#ifdef REGISTER_GUID_DEPRECATION
/* Register the deprecation in the header file, to make it clear to readers that this GUID
 * shouldn't be used for new SPDK GPT partitions.  We will never actually log this deprecation
 * though, since we are not recommending that users try to migrate existing partitions with the
 * old GUID to the new GUID. Wrap it in this REGISTER_GUID_DEPRECATION flag to avoid defining
 * this deprecation in multiple compilation units.
 */
SPDK_LOG_DEPRECATION_REGISTER(old_gpt_guid, "old gpt guid", "Never", 0)
#endif
#define SPDK_GPT_PART_TYPE_GUID_OLD SPDK_GPT_GUID(0x7c5222bd, 0x8f5d, 0x4087, 0x9c00, 0xbf9843c7b58c)

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
