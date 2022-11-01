/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Likely/unlikely branch prediction macros
 */

#ifndef SPDK_LIKELY_H
#define SPDK_LIKELY_H

#include "spdk/stdinc.h"

#define spdk_unlikely(cond)	__builtin_expect((cond), 0)
#define spdk_likely(cond)	__builtin_expect(!!(cond), 1)

#endif
