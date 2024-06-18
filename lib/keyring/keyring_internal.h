/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_INTERNAL_H
#define SPDK_KEYRING_INTERNAL_H

#include "spdk/json.h"
#include "spdk/keyring.h"

void keyring_dump_key_info(struct spdk_key *key, struct spdk_json_write_ctx *w);

#endif /* SPDK_KEYRING_INTERNAL_H */
