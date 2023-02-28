/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/string.h"

static __thread char strerror_message[64];

const char *
spdk_strerror(int errnum)
{
	spdk_strerror_r(errnum, strerror_message, sizeof(strerror_message));
	return strerror_message;
}
