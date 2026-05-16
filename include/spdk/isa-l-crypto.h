/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Google LLC
 *   All rights reserved.
 */

/** \file
 * Wrapper for isa-l_crypto headers
 */

#ifndef SPDK_ISAL_CRYPTO_H
#define SPDK_ISAL_CRYPTO_H

#include "spdk/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPDK_CONFIG_ISAL_CRYPTO
#error include/spdk/isa-l-crypto.h included when ISA-L-crypto is disabled!
#endif

#ifdef SPDK_CONFIG_ISAL_CRYPTO_INSTALLED
#include <isa-l-crypto/aes_xts.h>
#include <isa-l-crypto/isal_crypto_api.h>
#else
#include "../isa-l-crypto/include/isa-l_crypto/aes_xts.h"
#include "../isa-l-crypto/include/isa-l_crypto/isal_crypto_api.h"
#endif

#ifdef __cplusplus
}
#endif

#endif
