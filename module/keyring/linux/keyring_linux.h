/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_KEYRING_LINUX_H
#define SPDK_KEYRING_LINUX_H

#include "spdk/stdinc.h"

struct keyring_linux_opts {
	bool enable;
};

int keyring_linux_set_opts(struct keyring_linux_opts *opts);
void keyring_linux_get_opts(struct keyring_linux_opts *opts);

#endif /* SPDK_KEYRING_LINUX_H */
