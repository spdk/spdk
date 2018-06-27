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
#include "spdk/base64.h"

#define BASE64_ENC_BITMASK 0x3FUL
#define BASE64_PADDING_CHAR '='

static const char base64_enc_table[65] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

static const char base64_urfsafe_enc_table[65] =
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

static void
_spdk_base64_encode(char *text, const char *enc_table, const uint8_t *binary,
		    uint32_t binary_strlen)
{
	uint32_t bin_u32;

	while (binary_strlen >= 4) {
		bin_u32 = *(uint32_t *)binary;

		bin_u32 = htonl(bin_u32);

		*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 14) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 8) & BASE64_ENC_BITMASK];

		binary_strlen -= 3;
		binary += 3;
	}

	if (binary_strlen == 0) {
		goto out;
	}

	bin_u32 = 0;
	memcpy(&bin_u32, binary, binary_strlen);
	bin_u32 = htonl(bin_u32);

	if (binary_strlen == 3) {
		*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 14) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 8) & BASE64_ENC_BITMASK];
	} else if (binary_strlen == 2) {
		*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 14) & BASE64_ENC_BITMASK];
		*text++ = BASE64_PADDING_CHAR;
	} else if (binary_strlen == 1) {
		*text++ = enc_table[(bin_u32 >> 26) & BASE64_ENC_BITMASK];
		*text++ = enc_table[(bin_u32 >> 20) & BASE64_ENC_BITMASK];
		*text++ = BASE64_PADDING_CHAR;
		*text++ = BASE64_PADDING_CHAR;
	}

out:
	*text = '\0';
	return;
}

void
spdk_base64_encode(char *text, const uint8_t *binary, uint32_t binary_strlen)
{
	_spdk_base64_encode(text, base64_enc_table, binary, binary_strlen);
}

void
spdk_base64_urlsafe_encode(char *text, const uint8_t *binary, uint32_t binary_strlen)
{
	_spdk_base64_encode(text, base64_urfsafe_enc_table, binary, binary_strlen);
}

static int
_spdk_base64_decode(uint8_t *binary, uint32_t *_binary_len, const uint8_t *dec_table,
		    const char *text, uint32_t text_strlen)
{
	uint32_t tmp[4];
	uint32_t *b32;
	uint8_t *text_in;
	uint32_t tmp_strlen, tail_len = 0;

	/* strlen of text should be 4n */
	if (text_strlen == 0 || text_strlen % 4 != 0) {
		return -1;
	}

	/* Consider base64 padding */
	while (text[text_strlen - 1] == BASE64_PADDING_CHAR) {
		text_strlen--;
	}

	/* strlen of text without padding shouldn't be 4n+1 */
	if (text_strlen == 0 || text_strlen % 4 == 1) {
		return -1;
	}

	text_in = (uint8_t *) text;
	tmp_strlen = text_strlen;

	while (text_strlen >= 4) {
		tmp[0] = dec_table[*text_in++];
		tmp[1] = dec_table[*text_in++];
		tmp[2] = dec_table[*text_in++];
		tmp[3] = dec_table[*text_in++];

		if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255 || tmp[3] == 255) {
			return -1;
		}

		b32 = (uint32_t *)binary;
		*b32 = tmp[3] | tmp[2] << 6 | tmp[1] << 12 | tmp[0] << 18;
		*b32 = ntohl(*b32);
		*b32 >>= 8;

		binary += 3;
		text_strlen -= 4;
	}

	if (text_strlen == 0) {
		goto out;
	} else if (text_strlen == 1) {
		return -1;
	} else if (text_strlen == 2) {
		tmp[2] = 0;
		tail_len = 1;
	} else {
		tmp[2] = dec_table[text_in[2]];
		tail_len = 2;
	}

	tmp[1] = dec_table[text_in[1]];
	tmp[0] = dec_table[text_in[0]];

	if (tmp[0] == 255 || tmp[1] == 255 || tmp[2] == 255) {
		return -1;
	}

	tmp[0] = tmp[2] << 6 | tmp[1] << 12 | tmp[0] << 18;
	tmp[0] = ntohl(tmp[0]);
	tmp[0] >>= 8;
	memcpy(binary, (uint8_t *)&tmp[0], tail_len);

out:
	if (_binary_len != NULL) {
		*_binary_len = tmp_strlen / 4 * 3 + tail_len;
	}

	return 0;
}

int
spdk_base64_decode(uint8_t *binary, uint32_t *_binary_len,
		   const char *text, uint32_t text_strlen)
{
	return _spdk_base64_decode(binary, _binary_len, base64_dec_table, text, text_strlen);
}

int
spdk_base64_urlsafe_decode(uint8_t *binary, uint32_t *_binary_len,
			   const char *text, uint32_t text_strlen)
{
	return _spdk_base64_decode(binary, _binary_len, base64_urlsafe_dec_table, text, text_strlen);
}
