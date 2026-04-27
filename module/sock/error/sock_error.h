/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Nutanix Inc. All rights reserved.
 */

#ifndef SPDK_SOCK_ERROR_H
#define SPDK_SOCK_ERROR_H

#include "spdk/stdinc.h"
#include "spdk/module/sock/error.h"

struct sock_error_inject_opts {
	enum spdk_sock_error_operation operation;
	enum spdk_sock_error_type type;
	uint64_t count;
	uint64_t interval;
	int errcode;
	uint64_t match_len;
};

int sock_error_inject_error(struct sock_error_inject_opts *opts);

/**
 * Register the sock_error net_impl.
 * Requires the posix net_impl to be registered first, as it depends on it.
 */
int sock_error_register_impl(const char *base_impl_name);

#endif /* SPDK_SOCK_ERROR_H */
