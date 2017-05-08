/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/stdinc.h"

#include "iscsi/iscsi.h"
#include "iscsi/crc32c.h"

#ifndef USE_ISAL
#define SPDK_USE_CRC32C_TABLE
#endif

#ifdef SPDK_USE_CRC32C_TABLE
static uint32_t spdk_crc32c_table[256];

__attribute__((constructor)) static void
spdk_init_crc32c(void)
{
	int i, j;
	uint32_t val;

	for (i = 0; i < 256; i++) {
		val = i;
		for (j = 0; j < 8; j++) {
			if (val & 1) {
				val = (val >> 1) ^ SPDK_CRC32C_POLYNOMIAL_REFLECT;
			} else {
				val = (val >> 1);
			}
		}
		spdk_crc32c_table[i] = val;
	}
}
#endif /* SPDK_USE_CRC32C_TABLE */


#ifndef USE_ISAL
uint32_t
spdk_update_crc32c(const uint8_t *buf, size_t len, uint32_t crc)
{
	size_t s;
#ifndef SPDK_USE_CRC32C_TABLE
	int i;
	uint32_t val;
#endif /* SPDK_USE_CRC32C_TABLE */

	for (s = 0; s < len; s++) {
#ifdef SPDK_USE_CRC32C_TABLE
		crc = (crc >> 8) ^ spdk_crc32c_table[(crc ^ buf[s]) & 0xff];
#else
		val = buf[s];
		for (i = 0; i < 8; i++) {
			if ((crc ^ val) & 1) {
				crc = (crc >> 1) ^ SPDK_CRC32C_POLYNOMIAL_REFLECT;
			} else {
				crc = (crc >> 1);
			}
			val = val >> 1;
		}
#endif /* SPDK_USE_CRC32C_TABLE */
	}
	return crc;
}
#endif /* USE_ISAL */

uint32_t
spdk_fixup_crc32c(size_t total, uint32_t crc)
{
	uint8_t padding[ISCSI_ALIGNMENT];
	size_t pad_length;
	size_t rest;

	if (total == 0)
		return crc;
	rest = total % ISCSI_ALIGNMENT;
	if (rest != 0) {
		pad_length = ISCSI_ALIGNMENT;
		pad_length -= rest;
		if (pad_length > 0 && pad_length < sizeof padding) {
			memset(padding, 0, sizeof padding);
			crc = spdk_update_crc32c(padding, pad_length, crc);
		}
	}
	return crc;
}

uint32_t
spdk_crc32c(const uint8_t *buf, size_t len)
{
	uint32_t crc32c;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_update_crc32c(buf, len, crc32c);
	if ((len % ISCSI_ALIGNMENT) != 0) {
		crc32c = spdk_fixup_crc32c(len, crc32c);
	}
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}
