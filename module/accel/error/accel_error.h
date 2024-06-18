/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_ACCEL_ERROR_H
#define SPDK_ACCEL_ERROR_H

#include "spdk/accel.h"

enum accel_error_inject_type {
	ACCEL_ERROR_INJECT_DISABLE,
	ACCEL_ERROR_INJECT_CORRUPT,
	ACCEL_ERROR_INJECT_FAILURE,
	ACCEL_ERROR_INJECT_MAX,
};

struct accel_error_inject_opts {
	enum spdk_accel_opcode		opcode;
	enum accel_error_inject_type	type;
	uint64_t			count;
	uint64_t			interval;
	int				errcode;
};

int accel_error_inject_error(struct accel_error_inject_opts *opts);
const char *accel_error_get_type_name(enum accel_error_inject_type type);

#endif /* SPDK_ACCEL_ERROR_H */
