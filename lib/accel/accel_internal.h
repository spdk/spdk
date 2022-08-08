/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_ACCEL_INTERNAL_H
#define SPDK_INTERNAL_ACCEL_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/queue.h"
#include "spdk/config.h"

struct engine_info {
	struct spdk_json_write_ctx *w;
	const char *name;
	enum accel_opcode ops[ACCEL_OPC_LAST];
	uint32_t num_ops;
};

typedef void (*_accel_for_each_engine_fn)(struct engine_info *info);
void _accel_for_each_engine(struct engine_info *info, _accel_for_each_engine_fn fn);

#endif
