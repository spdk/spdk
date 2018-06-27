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
 *
 * Notes:
 *  Proceeding from left to right, a 24-bit input group is formed by concatenating 3 8-bit input groups
 *  Concatenate Rules:
 *      Byte order is LSB(Least Significant Byte first);
 *      Bit order is MSB(Most Significant Bit first);
 *
 *  | Byte0   | Byte1   | Byte2   |
 *  | 0x15    | 0x90    | 0x77    |
 *  |MSB   LSB|MSB   LSB|MSB   LSB|
 *  |0001 0101|1001 0000|0111 0111|
 *  | Text0| Text1 | Text2 | Text3 |
 *  |000101|01 1001|0000 01|11 0111|
 *  | F    | Z     | B     | 3     |
 */

/**
 * Base 64 Encoding with Standard Base64 Alphabet defined in RFC4684.
 *
 * \param _text Data buffer for encoded base64 string, it is allocated inside, so it should get freed later.
 * \param _text_strlen (Optional)Text string length, excluding the terminating null byte ('\0').
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 */
int spdk_base64_encode(char **_text, size_t *_text_strlen, const void *binary, size_t binary_len);

/**
 * Base 64 Encoding with URL and Filename Safe Alphabet.
 *
 * \param _text Data buffer for encoded base64 string.
 * \param _text_strlen (Optional)Text string length, excluding the terminating null byte ('\0').
 * \param binary Data buffer for binary to be encoded.
 * \param binary_len Length of binary buffer.
 */
int spdk_base64_urlsafe_encode(char **_text, size_t *_text_strlen, const void *binary,
			       size_t binary_len);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684..
 *
 * \param _binary Data buffer for decoded binary, it is allocated inside, so it should get freed later.
 * \param _binary_len Length of allocated and decoded binary buffer.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success
 */
int spdk_base64_decode(void **_binary, size_t *_binary_len, const char *text);

/**
 * Base 64 Decoding with Standard Base64 Alphabet defined in RFC4684..
 *
 * \param _binary Data buffer for decoded binary, it is allocated inside, so it should get freed later.
 * \param _binary_len Length of allocated and decoded binary buffer.
 * \param text Data buffer for base64 string to be decoded.
 *
 * \return 0 on success
 */
int spdk_base64_urlsafe_decode(void **_binary, size_t *_binary_len, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BASE64_H */
