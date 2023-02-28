/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_ASSERT_H
#define SPDK_INTERNAL_ASSERT_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/assert.h"

#if !defined(DEBUG) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
#define SPDK_UNREACHABLE() __builtin_unreachable()
#else
#define SPDK_UNREACHABLE() abort()
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_ASSERT_H */
