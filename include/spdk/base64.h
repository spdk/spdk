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
 * Following the Base64 part in RFC4648:
 * https://tools.ietf.org/html/rfc4648.html
 */

/**
 * Base 64 Encoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param text Output parameter for encoded base64 string address. It is allocated
 * by malloc() inside, so it should get freed by free() after its use.
 * \param text_strlen Output parameter for text string length, excluding the terminating
 * null byte ('\0'). If NULL, the string length is not returned.
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 *
 * \return 0 on success.
 * \return -ENOMEM if failed at memory allocation for text.
 * \return -EINVAL if text or binary is NULL, or binary_len <= 0.
 */
int spdk_base64_encode(char **text, size_t *text_strlen, const void *binary, size_t binary_len);

/**
 * Base 64 Encoding with URL and Filename Safe Alphabet.
 *
 * \param text Output parameter for encoded base64 string address. It is allocated
 * by malloc() inside, so it should get freed by free() after its use.
 * \param text_strlen Output parameter for text string length, excluding the terminating
 * null byte ('\0'). If NULL, the string length is not returned.
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 *
 * \return 0 on success.
 * \return -ENOMEM if failed at memory allocation for text.
 * \return -EINVAL if text or binary is NULL, or binary_len <= 0.
 */
int spdk_base64_urlsafe_encode(char **text, size_t *text_strlen, const void *binary,
			       size_t binary_len);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param binary Output parameter for decoded binary address. It is allocated
 * by malloc() inside, so it should get freed by free() after its use.
 * \param binary_len Output parameter for the length of allocated and decoded binary buffer.
 * If NULL, the buffer length is not returned.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -ENOMEM if failed at memory allocation for binary.
 * \return -EINVAL if binary or text is NULL, or content of text is illegal.
 */
int spdk_base64_decode(void **binary, size_t *binary_len, const char *text);

/**
 * Base 64 Decoding with URL and Filename Safe Alphabet.
 *
 * \param binary Output parameter for decoded binary address. It is allocated
 * by malloc() inside, so it should get freed by free() after its use.
 * \param binary_len Output parameter for the length of allocated and decoded binary buffer.
 * If NULL, the buffer length is not returned.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success.
 * \return -ENOMEM if failed at memory allocation for binary.
 * \return -EINVAL if binary or text is NULL, or content of text is illegal.
 */
int spdk_base64_urlsafe_decode(void **binary, size_t *binary_len, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BASE64_H */
