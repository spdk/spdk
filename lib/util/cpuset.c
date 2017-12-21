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
#include "spdk/log.h"

struct spdk_cpuset {
	char *str;
	uint8_t cpus[0];
};

#define SPDK_CPUSET_MASK_BYTES (SPDK_CPUSET_SIZE / 8)
#define SPDK_CPUSET_ALLOC_BYTES (SPDK_CPUSET_MASK_BYTES + sizeof(struct spdk_cpuset))

spdk_cpuset *
spdk_cpuset_alloc(void)
{
	return (spdk_cpuset *)calloc(SPDK_CPUSET_ALLOC_BYTES, 1);
}

void
spdk_cpuset_free(spdk_cpuset *set)
{
	if (set != NULL) {
		free(set->str);
	}
	free(set);
}

int
spdk_cpuset_cmp(const spdk_cpuset *set1, const spdk_cpuset *set2)
{
	assert(set1 != NULL);
	assert(set2 != NULL);
	return memcmp(set1->cpus, set2->cpus, SPDK_CPUSET_MASK_BYTES);
}

void
spdk_cpuset_copy(spdk_cpuset *set1, const spdk_cpuset *set2)
{
	assert(set1 != NULL);
	assert(set2 != NULL);
	memcpy(&set1->cpus, &set2->cpus, SPDK_CPUSET_MASK_BYTES);
}

void
spdk_cpuset_and(spdk_cpuset *set1, const spdk_cpuset *set2)
{
	int i;
	assert(set1 != NULL);
	assert(set2 != NULL);
	for (i = 0; i < SPDK_CPUSET_MASK_BYTES; i++) {
		set1->cpus[i] &= set2->cpus[i];
	}
}

void
spdk_cpuset_or(spdk_cpuset *set1, const spdk_cpuset *set2)
{
	int i;
	assert(set1 != NULL);
	assert(set2 != NULL);
	for (i = 0; i < SPDK_CPUSET_MASK_BYTES; i++) {
		set1->cpus[i] |= set2->cpus[i];
	}
}

void
spdk_cpuset_zero(spdk_cpuset *set)
{
	assert(set != NULL);
	memset(set->cpus, 0, SPDK_CPUSET_MASK_BYTES);
}

void
spdk_cpuset_set_cpu(spdk_cpuset *set, uint32_t cpu, bool state)
{
	assert(set != NULL);
	assert(cpu < SPDK_CPUSET_SIZE);
	if (state) {
		set->cpus[cpu / 8] |= (1ULL << (cpu % 8));
	} else {
		set->cpus[cpu / 8] &= ~(1ULL << (cpu % 8));
	}
}

bool
spdk_cpuset_get_cpu(const spdk_cpuset *set, uint32_t cpu)
{
	assert(set != NULL);
	assert(cpu < SPDK_CPUSET_SIZE);
	if (set->cpus[cpu / 8] & (1ULL << (cpu % 8))) {
		return true;
	}
	return 0;
}

uint32_t
spdk_cpuset_count(const spdk_cpuset *set)
{
	uint32_t count = 0;
	uint8_t n;
	int i;
	for (i = 0; i < SPDK_CPUSET_MASK_BYTES; i++) {
		n = set->cpus[i];
		while (n) {
			n &= (n - 1);
			count++;
		}
	}
	return count;
}

char *
spdk_cpuset_fmt(spdk_cpuset *set)
{
	uint32_t lcore, lcore_max = 0;
	int val, i, n;
	char *ptr;
	static const char *hex = "0123456789abcdef";

	assert(set != NULL);

	for (lcore = 0; lcore < SPDK_CPUSET_SIZE; lcore++) {
		if (spdk_cpuset_get_cpu(set, lcore)) {
			lcore_max = lcore;
		}
	}

	if (set->str == NULL) {
		set->str = (char *)malloc(SPDK_CPUSET_STR_MAX_LEN);
		assert(set->str != NULL);
	}

	ptr = set->str;
	n = lcore_max / 8;
	val = set->cpus[n];

	/* Store first number only if it is not leading zero */
	if ((val & 0xf0) != 0) {
		*(ptr++) = hex[(val & 0xf0) >> 4];
	}
	*(ptr++) = hex[val & 0x0f];

	for (i = n - 1; i >= 0; i--) {
		val = set->cpus[i];
		*(ptr++) = hex[(val & 0xf0) >> 4];
		*(ptr++) = hex[val & 0x0f];
	}
	*ptr = '\0';

	return set->str;
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
parse_list(const char *mask, spdk_cpuset *set)
{
	char *end;
	const char *ptr = mask;
	uint32_t lcore;
	uint32_t lcore_min, lcore_max;

	spdk_cpuset_zero(set);
	lcore_min = UINT32_MAX;

	ptr++;
	end = (char *)ptr;
	do {
		while (isblank(*ptr)) {
			ptr++;
		}
		if (*ptr == '\0' || *ptr == ']' || *ptr == '-' || *ptr == ',') {
			goto invalid_character;
		}

		errno = 0;
		lcore = strtoul(ptr, &end, 10);
		if (errno) {
			SPDK_ERRLOG("Conversion of core mask in '%s' failed\n", mask);
			return -1;
		}

		if (lcore >= SPDK_CPUSET_SIZE) {
			SPDK_ERRLOG("Core number %" PRIu32 " is out of range in '%s'\n", lcore, mask);
			return -1;
		}

		while (isblank(*end)) {
			end++;
		}

		if (*end == '-') {
			lcore_min = lcore;
		} else if (*end == ',' || *end == ']') {
			lcore_max = lcore;
			if (lcore_min == UINT32_MAX) {
				lcore_min = lcore;
			}
			if (lcore_min > lcore_max) {
				SPDK_ERRLOG("Invalid range of CPUs (%" PRIu32 " > %" PRIu32 ")\n",
					    lcore_min, lcore_max);
				return -1;
			}
			for (lcore = lcore_min; lcore <= lcore_max; lcore++) {
				spdk_cpuset_set_cpu(set, lcore, true);
			}
			lcore_min = UINT32_MAX;
		} else {
			goto invalid_character;
		}

		ptr = end + 1;

	} while (*end != ']');

	return 0;

invalid_character:
	if (*end == '\0') {
		SPDK_ERRLOG("Unexpected end of core list '%s'\n", mask);
	} else {
		SPDK_ERRLOG("Parsing of core list '%s' failed on character '%c'\n", mask, *end);
	}
	return -1;
}

static int
parse_mask(const char *mask, spdk_cpuset *set, int len)
{
	int i, j;
	char c;
	int val;
	uint32_t lcore = 0;

	if (mask[0] == '0' && (mask[1] == 'x' || mask[1] == 'X')) {
		mask += 2;
		len -= 2;
	}

	spdk_cpuset_zero(set);
	for (i = len - 1; i >= 0; i--) {
		c = mask[i];
		val = hex_value(c);
		if (val < 0) {
			/* Invalid character */
			SPDK_ERRLOG("Invalid character in core mask '%s' (%c)\n", mask, c);
			return -1;
		}
		for (j = 0; j < 4 && lcore < SPDK_CPUSET_SIZE; j++, lcore++) {
			if ((1 << j) & val) {
				spdk_cpuset_set_cpu(set, lcore, true);
			}
		}
	}

	return 0;
}

int
spdk_cpuset_parse(spdk_cpuset *set, const char *mask)
{
	int ret;
	size_t len;

	if (mask == NULL || set == NULL) {
		return -1;
	}

	while (isblank(*mask)) {
		mask++;
	}

	len = strlen(mask);
	while (len > 0 && isblank(mask[len - 1])) {
		len--;
	}

	if (len == 0) {
		return -1;
	}

	if (mask[0] == '[') {
		ret = parse_list(mask, set);
	} else {
		ret = parse_mask(mask, set, len);
	}

	return ret;
}
