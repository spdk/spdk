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
 * The QCOW image format specification definitions
 */

#ifndef SPDK_QCOW_SPEC_H
#define SPDK_QCOW_SPEC_H

#include "spdk/stdinc.h"

#include "spdk/assert.h"

#pragma pack(push, 1)

/* 4 bytes from Higher to low, 'Q', 'F', 'I', 0xfb */
#define SPDK_QCOW_MAGIC_NUM 0x514649fb

struct spdk_qcow_header {
	uint32_t magic;
	uint32_t version;

	uint64_t backing_file_offset;
	uint32_t backing_file_size;

	uint32_t cluster_bits;
	uint64_t size; /* in bytes */
	uint32_t crypt_method;

	uint32_t l1_size;
	uint64_t l1_table_offset;

	uint64_t refcount_table_offset;
	uint32_t refcount_table_clusters;

	uint32_t nb_snapshots;
	uint64_t snapshots_offset;
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_qcow_header) == 72, "size incorrect");

struct spdk_qcow_snap_header {
	/* header is 8 byte aligned */
	uint64_t l1_table_offset;

	uint32_t l1_size;
	uint16_t id_str_size;
	uint16_t name_size;

	uint32_t date_sec;
	uint32_t date_nsec;

	uint64_t vm_clock_nsec;

	uint32_t vm_state_size;
	uint32_t extra_data_size; /* for extension */
	/* extra data follows */
	/* id_str follows */
	/* name follows  */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_qcow_snap_header) == 40, "size incorrect");

#pragma pack(pop)

#endif
