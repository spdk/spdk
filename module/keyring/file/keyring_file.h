/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_FILE_H
#define SPDK_KEYRING_FILE_H

#include "spdk/keyring_module.h"

struct keyring_file_key_opts {
	char *name;
	char *path;
};

extern struct spdk_keyring_module g_keyring_file;

#endif
