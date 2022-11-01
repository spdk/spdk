/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
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
	IDXD_OPCODE_DECOMPRESS	= 66,
	IDXD_OPCODE_COMPRESS	= 67,
};

#ifdef __cplusplus
}
#endif

#endif /* __IDXD_INTERNAL_H__ */
