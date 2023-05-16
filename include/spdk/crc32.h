/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * CRC-32 utility functions
 */

#ifndef SPDK_CRC32_H
#define SPDK_CRC32_H

#include "spdk/stdinc.h"
#include "spdk/config.h"

#define SPDK_CRC32_SIZE_BYTES 4

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculate a partial CRC-32 IEEE checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32 value.
 * \return Updated CRC-32 value.
 */
uint32_t spdk_crc32_ieee_update(const void *buf, size_t len, uint32_t crc);

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_update(const void *buf, size_t len, uint32_t crc);

/**
 * Calculate a partial CRC-32C checksum.
 *
 * \param iov Data buffer vectors to checksum.
 * \param iovcnt size of iov parameter.
 * \param crc32c Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_iov_update(struct iovec *iov, int iovcnt, uint32_t crc32c);

/**
 * Calculate a CRC-32C checksum, for NVMe Protection Information
 *
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32C value.
 * \return Updated CRC-32C value.
 */
uint32_t spdk_crc32c_nvme(const void *buf, size_t len, uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_CRC32_H */
