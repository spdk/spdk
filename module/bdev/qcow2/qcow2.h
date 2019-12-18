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

#ifndef SPDK_INTERNAL_QCOW_H
#define SPDK_INTERNAL_QCOW_H

#include "spdk/stdinc.h"

#include "spdk/qcow_spec.h"

#define SPDK_QCOW2_BUFFER_SIZE 512  /* 512 bytes */

enum spdk_qcow2_parse_phase {
	SPDK_QCOW2_PARSE_PHASE_INVALID = 0,
	SPDK_QCOW2_PARSE_PHASE_QCOW_HEADER,
	SPDK_QCOW2_PARSE_PHASE_SNAPSHOT_HEADER,
};

struct spdk_qcow2 {
	enum spdk_qcow2_parse_phase parse_phase;
	unsigned char *buf;
	struct spdk_qcow_header header;
	uint64_t buf_size;
	uint64_t lba_start;
	uint64_t lba_end;
	uint64_t total_sectors;
	uint32_t sector_size;
};

int spdk_qcow2_parse_header(struct spdk_qcow2 *qcow2);
int spdk_qcow2_parse_mapping_table(struct spdk_qcow2 *qcow2);

#endif  /* SPDK_INTERNAL_GPT_H */
