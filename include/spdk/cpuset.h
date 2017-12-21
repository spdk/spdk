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

#ifndef SPDK_CPUSET_H
#define SPDK_CPUSET_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_CPUSET_SIZE 1024
#define SPDK_CPUSET_STR_MAX_LEN (SPDK_CPUSET_SIZE / 4 + 1)

typedef struct spdk_cpuset spdk_cpuset;

/**
 * \brief Allocate CPU set object.
 */
spdk_cpuset *spdk_cpuset_alloc(void);

/**
 * \brief Free allocated CPU set.
 */
void spdk_cpuset_free(spdk_cpuset *set);

/**
 * \brief Compare two CPU sets. Returns zero if equal.
 */
int spdk_cpuset_cmp(const spdk_cpuset *set1, const spdk_cpuset *set2);

/**
 * \brief Copy the content of CPU set to another.
 */
void spdk_cpuset_copy(spdk_cpuset *dst, const spdk_cpuset *src);

/**
 * \brief Perform AND operation on two CPU set. The result is stored in dst.
 */
void spdk_cpuset_and(spdk_cpuset *dst, const spdk_cpuset *src);

/**
 * \brief Perform OR operation on two CPU set. The result is stored in dst.
 */
void spdk_cpuset_or(spdk_cpuset *dst, const spdk_cpuset *src);

/**
 * \brief Clear all CPUs in CPU set.
 */
void spdk_cpuset_zero(spdk_cpuset *set);

/**
 * \brief Set or clear CPU state in CPU set.
 */
void spdk_cpuset_set_cpu(spdk_cpuset *set, uint32_t cpu, bool state);

/**
 * \brief Get the state of CPU in CPU set.
 */
bool spdk_cpuset_get_cpu(const spdk_cpuset *set, uint32_t cpu);

/**
 * \brief Get the number of CPUs that are set in CPU set.
 */
uint32_t spdk_cpuset_count(const spdk_cpuset *set);

/**
 * \brief Convert a CPU set to hex string. Buffer to store a string is
 * dynamically allocated internally and freed with cpuset object.
 */
char *spdk_cpuset_fmt(spdk_cpuset *set);

/**
 * \brief Convert a string containing a CPU core mask into a cpuset. By default
 * hexadecimal value is used or as CPU list enclosed in square brackets
 * defined as: 'c1[-c2][,c3[-c4],...]'
 */
int spdk_cpuset_parse(spdk_cpuset *set, const char *mask);

#ifdef __cplusplus
}
#endif
#endif /* SPDK_CPUSET_H */
