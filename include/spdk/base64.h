/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Base64 utility functions
 */

#ifndef SPDK_BASE64_H
#define SPDK_BASE64_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Following the Base64 part in RFC4648:
 * https://tools.ietf.org/html/rfc4648.html
 */

/**
 * Calculate strlen of encoded Base64 string based on raw buffer length.
 *
 * \param raw_len Length of raw buffer.
 * \return Encoded Base64 string length, excluding the terminating null byte ('\0').
 */
static inline size_t spdk_base64_get_encoded_strlen(size_t raw_len)
{
	return (raw_len + 2) / 3 * 4;
}

/**
 * Calculate length of raw buffer based on strlen of encoded Base64.
 *
 * This length will be the max possible decoded len. The exact decoded length could be
 * shorter depending on if there was padding in the Base64 string.
 *
 * \param encoded_strlen Length of encoded Base64 string, excluding terminating null
 * byte ('\0').
 * \return Length of raw buffer.
 */
static inline size_t spdk_base64_get_decoded_len(size_t encoded_strlen)
{
	/* text_strlen and raw_len should be (4n,3n), (4n+2, 3n+1) or (4n+3, 3n+2) */
	return encoded_strlen / 4 * 3 + ((encoded_strlen % 4 + 1) / 2);
}

/**
 * Base 64 Encoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param dst Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_encoded_strlen(src_len).
 * \param src Raw data buffer to be encoded.
 * \param src_len Length of raw data buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if dst or src is NULL, or binary_len <= 0.
 */
int spdk_base64_encode(char *dst, const void *src, size_t src_len);

/**
 * Base 64 Encoding with URL and Filename Safe Alphabet.
 *
 * \param dst Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_encoded_strlen(src_len).
 * \param src Raw data buffer to be encoded.
 * \param src_len Length of raw data buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if dst or src is NULL, or binary_len <= 0.
 */
int spdk_base64_urlsafe_encode(char *dst, const void *src, size_t src_len);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param dst Buffer address of decoded raw data. Its length should be enough
 * to contain decoded raw data, so it needs to be at least as long as
 * spdk_base64_get_decoded_len(encoded_strlen). If NULL, only dst_len will be populated
 * indicating the exact decoded length.
 * \param dst_len Output parameter for the length of actual decoded raw data.
 * If NULL, the actual decoded length won't be returned.
 * \param src Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if src is NULL, or content of src is illegal.
 */
int spdk_base64_decode(void *dst, size_t *dst_len, const char *src);

/**
 * Base 64 Decoding with URL and Filename Safe Alphabet.
 *
 * \param dst Buffer address of decoded raw data. Its length should be enough
 * to contain decoded raw data, so it needs to be at least as long as
 * spdk_base64_get_decoded_len(encoded_strlen). If NULL, only dst_len will be populated
 * indicating the exact decoded length.
 * \param dst_len Output parameter for the length of actual decoded raw data.
 * If NULL, the actual decoded length won't be returned.
 * \param src Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if src is NULL, or content of src is illegal.
 */
int spdk_base64_urlsafe_decode(void *dst, size_t *dst_len, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BASE64_H */
