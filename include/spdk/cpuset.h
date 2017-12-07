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

#ifndef _SPDK_CPUSET_H
#define _SPDK_CPUSET_H

#include "spdk/stdinc.h"

#if defined(__linux__)
typedef	cpu_set_t spdk_cpuset_t;
#elif defined(__FreeBSD__)
#include <pthread_np.h>
typedef cpuset_t spdk_cpuset_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Convert a CPU set to hex string.
 */
char *spdk_core_mask_hex(const spdk_cpuset_t *cpumask, char *mask, int n);

/**
 * \brief Convert a string containing a CPU core mask into a cpuset. By default
 * hexadecimal value is used or as CPU list enclosed in square brackets
 * defined as: 'c1[-c2][,c3[-c4],...]'
 */
int spdk_parse_core_mask(const char *mask, spdk_cpuset_t *cpumask);

#ifdef __cplusplus
}
#endif
#endif /* _SPDK_CPUSET_H */
