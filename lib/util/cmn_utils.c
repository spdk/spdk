/*-
 *   BSD LICENSE
 *
 *   Copyright (c) NetApp, Inc.
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

#include <spdk/cmn_utils.h>
#include <rte_cycles.h>

/* Const values for logBase10 function */
#define P_0  1
#define P_1  10
#define P_2  100
#define P_3  1000
#define P_4  10000
#define P_5  100000
#define P_6  1000000
#define P_7  10000000
#define P_8  100000000
#define P_9  1000000000
#define P_10 10000000000ULL
#define P_11 100000000000ULL
#define P_12 1000000000000ULL
#define P_13 10000000000000ULL
#define P_14 100000000000000ULL
#define P_15 1000000000000000ULL
#define P_16 10000000000000000ULL
#define P_17 100000000000000000ULL
#define P_18 1000000000000000000ULL
#define P_19 10000000000000000000ULL

uint64_t
spdk_timestamp_ticks(void)
{
	return rte_get_tsc_cycles();
}

uint64_t
spdk_ticks_to_nsec(uint64_t ticks)
{
	return (ticks + (hz / SEC_TO_NANOSEC) - 1) / (hz / SEC_TO_NANOSEC);
}

uint32_t
spdk_percent_fn(uint64_t mul1, uint64_t divisor)
{
	if (!divisor)
		return 0;

	return (uint32_t)((mul1 * 100) / divisor);
}

uint64_t
spdk_power_fn(uint32_t base, uint64_t exp)
{
	uint64_t i;
	uint64_t val = 1;

	if (base == 2)
		return exp > 0 && exp < 64 ? val << exp : 1;

	if (exp > 0 && exp < 21) {
		for (i = 0; i < exp; i++)
			val *= base;
	}
	return val;
}

uint32_t
spdk_floor_log2(uint64_t val)
{
	uint32_t log = 0;
	if (val > 0) {
		uint64_t mostsigbit = 0x8000000000000000ULL;
		uint32_t i = 0;
		uint32_t bits = sizeof(uint64_t) * 8 - 1;
		for (i = 0; i < bits; i++) {
			if (val << i & mostsigbit)
				break;
		}
		log = bits - i;
	}
	return log;
}

uint32_t
spdk_floor_log10(uint64_t val)
{
	uint32_t log = 0;

	if (val < P_16) {
		if (val < P_8) {
			if (val < P_4) {
				if (val < P_2)
					log = (val < P_1) ? 0 : 1;
				else
					log = (val < P_3) ? 2 : 3;
			} else {
				if (val < P_6)
					log = (val < P_5) ? 4 : 5;
				else
					log = (val < P_7) ? 6 : 7;
			}
		} else {
			if (val < P_12) {
				if (val < P_10)
					log = (val < P_9) ? 8 : 9;
				else
					log = (val < P_11) ? 10 : 11;
			} else {
				if (val < P_14)
					log = (val < P_13) ? 12 : 13;
				else
					log = (val < P_15) ? 14 : 15;
			}
		}
	} else {
		if (val < P_18)
			log = (val < P_17) ? 16 : 17;
		else
			log = (val < P_19) ? 18 : 19;
	}
	return log;
}
