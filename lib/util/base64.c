/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "spdk/stdinc.h"
#include "spdk/endian.h"
#include "spdk/base64.h"

#define BASE64_ENC_BITMASK 0x3FUL
#define BASE64_PADDING_CHAR '='

static const char base64_enc_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

static const char base64_urfsafe_enc_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789-_";


static const uint8_t
base64_dec_table[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,
	255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static const uint8_t
base64_urlsafe_dec_table[] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255,
	52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
	255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,  63,
	255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

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
 * \param text_strlen Length of text string, excluding the padding byte ('=') and terminating null byte ('\0').
 * \return Length of binary buffer.
 */
static inline size_t spdk_base64_get_binary_len(size_t text_strlen)
{
	/* text_strlen and binary_len should be (4n,3n), (4n+2, 3n+1) or (4n+3, 3n+2) */
	return text_strlen / 4 * 3 + ((text_strlen % 4 + 1) / 2);
}

static int
_spdk_base64_encode(char **_text, size_t *_text_strlen, const char *enc_table,
		    const void *binary, size_t binary_len)
{
	uint32_t bin_u32;
	size_t text_strlen;
	char *text;

	if (!_text || !binary || binary_len <= 0) {
		return -EINVAL;
	}

	text_strlen = spdk_base64_get_text_strlen(binary_len);

	text = malloc(text_strlen + 1);
	if (!text) {
		return -ENOMEM;
	}

	*_text = text;
	if (_text_strlen) {
		*_text_strlen = text_strlen;
	}

	while (binary_len >= 4) {
		bin_u32 = from_be32(binary);

		*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 14) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 8) & BASE64_ENC_BITMASK];

		binary_len -= 3;
		binary += 3;
	}

	if (binary_len == 0) {
		goto out;
	}

	bin_u32 = 0;
	memcpy(&bin_u32, binary, binary_len);
	bin_u32 = from_be32(&bin_u32);

	*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
	*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
	*text++ = (binary_len >= 2) ? enc_table[(bin_u32 >> 14) & BASE64_ENC_BITMASK] : BASE64_PADDING_CHAR;
	*text++ = (binary_len == 3) ? enc_table[(bin_u32 >> 8) & BASE64_ENC_BITMASK] : BASE64_PADDING_CHAR;

out:
	*text = '\0';
	return 0;
}

int
spdk_base64_encode(char **_text, size_t *_text_strlen, const void *binary, size_t binary_len)
{
	return _spdk_base64_encode(_text, _text_strlen, base64_enc_table, binary, binary_len);
}

int
spdk_base64_urlsafe_encode(char **_text, size_t *_text_strlen, const void *binary,
			   size_t binary_len)
{
	return _spdk_base64_encode(_text, _text_strlen, base64_urfsafe_enc_table, binary, binary_len);
}

static int
_spdk_base64_decode(void **_binary, size_t *_binary_len, const uint8_t *dec_table,
		    const char *text)
{
	size_t text_strlen, binary_len;
	size_t tail_len = 0;
	char *binary, *binary_tmp;
	const uint8_t *text_in;
	uint32_t tmp[4];
	int i;

	if (!_binary || !text) {
		return -EINVAL;
	}

	text_strlen = strlen(text);

	/* strlen of text should be 4n */
	if (text_strlen == 0 || text_strlen % 4 != 0) {
		return -EINVAL;
	}

	/* Consider Base64 padding, it at most has 2 padding characters. */
	for (i = 0; i < 2; i++) {
		if (text[text_strlen - 1] != BASE64_PADDING_CHAR) {
			break;
		}
		text_strlen--;
	}

	/* strlen of text without padding shouldn't be 4n+1 */
	if (text_strlen == 0 || text_strlen % 4 == 1) {
		return -EINVAL;
	}

	binary_len = spdk_base64_get_binary_len(text_strlen);

	binary = malloc(binary_len);
	if (!binary) {
		return -ENOMEM;
	}

	binary_tmp = binary;
	text_in = (const uint8_t *) text;

	/* space of binary can be used by to_be32 */
	while (text_strlen > 4) {
		tmp[0] = dec_table[*text_in++];
		tmp[1] = dec_table[*text_in++];
		tmp[2] = dec_table[*text_in++];
		tmp[3] = dec_table[*text_in++];

		if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255 || tmp[3] == 255) {
			free(binary_tmp);
			return -EINVAL;
		}

		to_be32(binary, tmp[3] << 8 | tmp[2] << 14 | tmp[1] << 20 | tmp[0] << 26);

		binary += 3;
		text_strlen -= 4;
	}

	/* space of binary is not enough to be used by to_be32 */
	tmp[0] = dec_table[text_in[0]];
	tmp[1] = dec_table[text_in[1]];
	tmp[2] = (text_strlen >= 3) ? dec_table[text_in[2]] : 0;
	tmp[3] = (text_strlen == 4) ? dec_table[text_in[3]] : 0;
	tail_len = text_strlen - 1;

	if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255 || tmp[3] == 255) {
		free(binary_tmp);
		return -EINVAL;
	}

	to_be32(&tmp[3], tmp[3] << 8 | tmp[2] << 14 | tmp[1] << 20 | tmp[0] << 26);
	memcpy(binary, (uint8_t *)&tmp[3], tail_len);

	/* Assign pointers */
	if (_binary_len) {
		*_binary_len = binary_len;
	}
	*_binary = binary_tmp;

	return 0;
}

int
spdk_base64_decode(void **_binary, size_t *_binary_len, const char *text)
{
	return _spdk_base64_decode(_binary, _binary_len, base64_dec_table, text);
}

int
spdk_base64_urlsafe_decode(void **_binary, size_t *_binary_len, const char *text)
{
	return _spdk_base64_decode(_binary, _binary_len, base64_urlsafe_dec_table, text);
}
