/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
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
