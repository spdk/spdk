/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
 * Calculate strlen of text based on binary's length.
 *
 * \param binary_len Length of binary buffer.
 * \return Text string length, excluding the terminating null byte ('\0').
 */
static inline size_t spdk_base64_get_text_strlen(size_t binary_len)
{
	return (binary_len + 2) / 3 * 4;
}

/**
 * Calculate length of binary buffer based on text's strlen.
 *
 * \param text_strlen Length of text string, excluding terminating null byte ('\0').
 * \return Length of binary buffer.
 */
static inline size_t spdk_base64_get_binary_len(size_t text_strlen)
{
	/* text_strlen and binary_len should be (4n,3n), (4n+2, 3n+1) or (4n+3, 3n+2) */
	return text_strlen / 4 * 3 + ((text_strlen % 4 + 1) / 2);
}

/**
 * Following the Base64 part in RFC4648:
 * https://tools.ietf.org/html/rfc4648.html
 */

/**
 * Base 64 Encoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param text Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_text_strlen(binary_len).
 * \param text_strlen Output parameter for text string length, excluding the terminating
 * null byte ('\0'). If NULL, the string length is not returned.
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if text or binary is NULL, or binary_len <= 0.
 */
int spdk_base64_encode(char *text, size_t *text_strlen, const void *binary, size_t binary_len);

/**
 * Base 64 Encoding with URL and Filename Safe Alphabet.
 *
 * \param text Buffer address of encoded Base64 string. Its length should be enough
 * to contain Base64 string and the terminating null byte ('\0'), so it needs to be at
 * least as long as 1 + spdk_base64_get_text_strlen(binary_len).
 * \param text_strlen Output parameter for text string length, excluding the terminating
 * null byte ('\0'). If NULL, the string length is not returned.
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 *
 * \return 0 on success.
 * \return -EINVAL if text or binary is NULL, or binary_len <= 0.
 */
int spdk_base64_urlsafe_encode(char *text, size_t *text_strlen, const void *binary,
			       size_t binary_len);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param binary Buffer address of decoded binary. Its length should be enough
 * to contain decoded binary string, so it needs to be at least as long as
 * spdk_base64_get_binary_len(text_strlen).
 * \param binary_len Output parameter for the length of allocated and decoded binary buffer.
 * If NULL, the buffer length is not returned.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if binary or text is NULL, or content of text is illegal.
 */
int spdk_base64_decode(void *binary, size_t *binary_len, const char *text);

/**
 * Base 64 Decoding with URL and Filename Safe Alphabet.
 *
 * \param binary Buffer address of decoded binary. Its length should be enough
 * to contain decoded binary string, so it needs to be at least as long as
 * spdk_base64_get_binary_len(text_strlen).
 * \param binary_len Output parameter for the length of allocated and decoded binary buffer.
 * If NULL, the buffer length is not returned.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -EINVAL if binary or text is NULL, or content of text is illegal.
 */
int spdk_base64_urlsafe_decode(void *binary, size_t *binary_len, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BASE64_H */
