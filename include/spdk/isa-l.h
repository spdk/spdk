/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Google LLC
 *   All rights reserved.
 */

/** \file
 * Wrapper for isa-l headers
 */

#ifndef SPDK_ISAL_H
#define SPDK_ISAL_H

#include "spdk/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPDK_CONFIG_ISAL
#error include/spdk/isa-l.h included when ISA-L is disabled!
#endif

#define SPDK_HAVE_ISAL
#ifdef SPDK_CONFIG_ISAL_INSTALLED
#include <isa-l/crc.h>
#include <isa-l/crc64.h>
#include <isa-l/igzip_lib.h>
#include <isa-l/raid.h>
#else
#include "../isa-l/include/crc.h"
#include "../isa-l/include/crc64.h"
#include "../isa-l/include/igzip_lib.h"
#include "../isa-l/include/raid.h"
#endif

#ifdef __cplusplus
}
#endif

#endif
