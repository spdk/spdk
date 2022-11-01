/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Runtime and compile-time assert macros
 */

#ifndef SPDK_ASSERT_H
#define SPDK_ASSERT_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef static_assert
#define SPDK_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
/**
 * Compatibility wrapper for static_assert.
 *
 * This won't actually enforce the condition when compiled with an environment that doesn't support
 * C11 static_assert; it is only intended to allow end users with old compilers to build the package.
 *
 * Developers should use a recent compiler that provides static_assert.
 */
#define SPDK_STATIC_ASSERT(cond, msg)
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPDK_ASSERT_H */
