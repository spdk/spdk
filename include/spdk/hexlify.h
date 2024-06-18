/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_HEXLIFY_H
#define SPDK_HEXLIFY_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a binary array to hexlified string terminated by zero.
 *
 * \param bin A binary array pointer.
 * \param len Length of the binary array.
 * \return Pointer to hexlified version of @bin or NULL on failure.
 */
char *spdk_hexlify(const char *bin, size_t len);

/**
 * Convert hexlified string to binary array of size strlen(hex) / 2.
 *
 * \param hex A hexlified string terminated by zero.
 * \return Binary array pointer or NULL on failure.
 */
char *spdk_unhexlify(const char *hex);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_HEXLIFY_H */
