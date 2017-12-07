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

#include "spdk/cpuset.h"

char *
spdk_core_mask_hex(const spdk_cpuset_t *cpumask, char *mask, size_t sz)
{
	uint32_t lcore, lcore_max = 0;
	int val, i;
	char *ptr = mask;

	const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
			       'a', 'b', 'c', 'd', 'e', 'f'
			     };

	for (lcore = 0; lcore < SPDK_CPU_SETSIZE; lcore++) {
		if (SPDK_CPU_ISSET(lcore, cpumask)) {
			lcore_max = lcore;
		}
	}

	if (lcore_max >= (sz - 1) * 4) {
		lcore_max = (sz - 1) * 4 - 1;
	}

	val = 0;
	for (i = lcore_max; i >= 0; i--) {
		val <<= 1;
		if (SPDK_CPU_ISSET(i, cpumask)) {
			val |= 1;
		}
		if (i % 4 == 0) {
			*ptr = hex[val];
			val = 0;
			ptr++;
		}
	}
	*ptr = '\0';

	return mask;
}

static int
hex_value(uint8_t c)
{
#define V(x, y) [x] = y + 1
	static const int8_t val[256] = {
		V('0', 0), V('1', 1), V('2', 2), V('3', 3), V('4', 4),
		V('5', 5), V('6', 6), V('7', 7), V('8', 8), V('9', 9),
		V('A', 0xA), V('B', 0xB), V('C', 0xC), V('D', 0xD), V('E', 0xE), V('F', 0xF),
		V('a', 0xA), V('b', 0xB), V('c', 0xC), V('d', 0xD), V('e', 0xE), V('f', 0xF),
	};
#undef V

	return val[c] - 1;
}

static int
parse_core_list(const char *mask, spdk_cpuset_t *cpumask, int len)
{
	char *end;
	uint32_t lcore;
	uint32_t lcore_min, lcore_max;

	if (len == 0) {
		return -1;
	}

	SPDK_CPU_ZERO(cpumask);
	lcore_min = UINT32_MAX;

	do {
		while (isblank(*mask)) {
			mask++;
		}
		if (*mask == '\0' || *mask == ']' || *mask == '-' || *mask == ',') {
			return -1;
		}

		errno = 0;
		lcore = strtoul(mask, &end, 10);
		if (errno || end == NULL) {
			return -1;
		}

		while (isblank(*end)) {
			end++;
		}

		if (*end == '-') {
			lcore_min = lcore;
		} else if ((*end == ',') || (*end == ']')) {
			lcore_max = lcore;
			if (lcore_min == UINT32_MAX) {
				lcore_min = lcore;
			}
			if (lcore_min > lcore_max) {
				return -1;
			}
			for (lcore = lcore_min; lcore <= lcore_max; lcore++) {
				SPDK_CPU_SET(lcore, cpumask);
			}
			lcore_min = UINT32_MAX;
		} else {
			return -1;
		}

		mask = end + 1;

	} while (*end != ']');

	return 0;
}

static int
parse_core_mask(const char *mask, spdk_cpuset_t *cpumask, int len)
{
	int i, j;
	char c;
	int val;
	uint32_t lcore = 0;

	if (len > 1 && mask[0] == '0' && (mask[1] == 'x' || mask[1] == 'X')) {
		mask += 2;
		len -= 2;
	}
	if (len == 0) {
		return -1;
	}

	SPDK_CPU_ZERO(cpumask);
	for (i = len - 1; i >= 0; i--) {
		c = mask[i];
		val = hex_value(c);
		if (val < 0) {
			/* Invalid character */
			return -1;
		}
		for (j = 0; j < 4 && lcore < SPDK_CPU_SETSIZE; j++, lcore++) {
			if ((1 << j) & val) {
				SPDK_CPU_SET(lcore, cpumask);
			}
		}
	}

	return 0;
}

int
spdk_parse_core_mask(const char *mask, spdk_cpuset_t *cpumask)
{
	int len, ret;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	while (isblank(*mask)) {
		mask++;
	}

	len = strlen(mask);
	while ((len > 0) && isblank(mask[len - 1])) {
		len--;
	}

	if (len > 0 && mask[0] == '[') {
		mask++;
		ret = parse_core_list(mask, cpumask, len);
	} else {
		ret = parse_core_mask(mask, cpumask, len);
	}

	return ret;
}
