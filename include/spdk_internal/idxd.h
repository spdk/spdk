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

#ifndef __IDXD_INTERNAL_H__
#define __IDXD_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/idxd.h"
#include "spdk/queue.h"
#include "spdk/mmio.h"
#include "spdk/bit_array.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDXD_MAX_CONFIG_NUM 1

enum dsa_opcode {
	IDXD_OPCODE_NOOP	= 0,
	IDXD_OPCODE_BATCH	= 1,
	IDXD_OPCODE_DRAIN	= 2,
	IDXD_OPCODE_MEMMOVE	= 3,
	IDXD_OPCODE_MEMFILL	= 4,
	IDXD_OPCODE_COMPARE	= 5,
	IDXD_OPCODE_COMPVAL	= 6,
	IDXD_OPCODE_CR_DELTA	= 7,
	IDXD_OPCODE_AP_DELTA	= 8,
	IDXD_OPCODE_DUALCAST	= 9,
	IDXD_OPCODE_CRC32C_GEN	= 16,
	IDXD_OPCODE_COPY_CRC	= 17,
	IDXD_OPCODE_DIF_CHECK	= 18,
	IDXD_OPCODE_DIF_INS	= 19,
	IDXD_OPCODE_DIF_STRP	= 20,
	IDXD_OPCODE_DIF_UPDT	= 21,
	IDXD_OPCODE_CFLUSH	= 32,
};

#ifdef __cplusplus
}
#endif

#endif /* __IDXD_INTERNAL_H__ */
