/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_ACCEL_INTERNAL_H
#define SPDK_INTERNAL_ACCEL_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/queue.h"
#include "spdk/config.h"

struct module_info {
	struct spdk_json_write_ctx *w;
	const char *name;
	enum accel_opcode ops[ACCEL_OPC_LAST];
	uint32_t num_ops;
};

typedef void (*_accel_for_each_module_fn)(struct module_info *info);
void _accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn);
int _accel_get_opc_name(enum accel_opcode opcode, const char **opcode_name);
void _accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key);
void _accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w);

#endif
