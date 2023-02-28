/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-16 utility functions
 */

#ifndef SPDK_CRC16_H
#define SPDK_CRC16_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * T10-DIF CRC-16 polynomial
 */
#define SPDK_T10DIF_CRC16_POLYNOMIAL 0x8bb7u

/**
 * Calculate T10-DIF CRC-16 checksum.
 *
 * \param init_crc Initial CRC-16 value.
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \return CRC-16 value.
 */
uint16_t spdk_crc16_t10dif(uint16_t init_crc, const void *buf, size_t len);

/**
 * Calculate T10-DIF CRC-16 checksum and copy data.
 *
 * \param init_crc Initial CRC-16 value.
 * \param dst Destination data buffer for copy.
 * \param src Source data buffer for CRC calculation and copy.
 * \param len Length of buffer in bytes.
 * \return CRC-16 value.
 */
uint16_t spdk_crc16_t10dif_copy(uint16_t init_crc, uint8_t *dst, uint8_t *src,
				size_t len);
#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC16_H */
