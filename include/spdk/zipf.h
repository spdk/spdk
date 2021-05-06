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

/** \file
 * Zipf random number distribution
 */

#ifndef SPDK_ZIPF_H
#define SPDK_ZIPF_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_zipf;

/**
 * Create a zipf random number generator.
 *
 * Numbers from [0, range) will be returned by the generator when
 * calling \ref spdk_zipf_generate.
 *
 * \param range Range of values for the zipf distribution.
 * \param theta Theta distribution parameter.
 * \param seed Seed value for the random number generator.
 *
 * \return a pointer to the new zipf generator.
 */
struct spdk_zipf *spdk_zipf_create(uint64_t range, double theta, uint32_t seed);

/**
 * Free a zipf generator and set the pointer to NULL.
 *
 * \param zipfp Zipf generator to free.
 */
void spdk_zipf_free(struct spdk_zipf **zipfp);

/**
 * Generate a value from the zipf generator.
 *
 * \param zipf Zipf generator to generate the value from.
 *
 * \return value in the range [0, range)
 */
uint64_t spdk_zipf_generate(struct spdk_zipf *zipf);

#ifdef __cplusplus
}
#endif

#endif
