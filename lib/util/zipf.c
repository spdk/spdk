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
#include "spdk/util.h"
#include "spdk/zipf.h"

struct spdk_zipf {
	uint64_t	range;
	double		alpha;
	double		eta;
	double		theta;
	double		zetan;
	double		val1_limit;
	uint32_t	seed;
};

static double
zeta_increment(uint64_t n, double theta)
{
	return pow((double) 1.0 / (n + 1), theta);
}

static double
zeta(uint64_t range, double theta)
{
	double zetan = 0;
	double inc1, inc2;
	uint64_t i, calc, count;
	const uint32_t ZIPF_MAX_ZETA_CALC = 10 * 1000 * 1000;
	const uint32_t ZIPF_ZETA_ESTIMATE = 1 * 1000 * 1000;

	/* Cumulate zeta discretely for the first ZIPF_MAX_ZETA_CALC
	 * entries in the range.
	 */
	calc = spdk_min(ZIPF_MAX_ZETA_CALC, range);
	for (i = 0; i < calc; i++) {
		zetan += zeta_increment(i, theta);
	}

	/* For the remaining values in the range, increment zetan
	 * with an approximation for every ZIPF_ZETA_ESTIMATE
	 * entries.  We will take an average of the increment
	 * for (i) and (i + ZIPF_ZETA_ESTIMATE), and then multiply
	 * that by ZIPF_ZETA_ESTIMATE.
	 *
	 * Of course, we'll cap ZIPF_ZETA_ESTIMATE to something
	 * smaller if necessary at the end of the range.
	 */
	while (i < range) {
		count = spdk_min(ZIPF_ZETA_ESTIMATE, range - i);
		inc1 = zeta_increment(i, theta);
		inc2 = zeta_increment(i + count, theta);
		zetan += (inc1 + inc2) * count / 2;
		i += count;
	}

	return zetan;
}

struct spdk_zipf *
spdk_zipf_create(uint64_t range, double theta, uint32_t seed)
{
	struct spdk_zipf *zipf;

	zipf = calloc(1, sizeof(*zipf));
	if (zipf == NULL) {
		return NULL;
	}

	zipf->range = range;
	zipf->seed = seed;

	zipf->theta = theta;
	zipf->alpha = 1.0 / (1.0 - zipf->theta);
	zipf->zetan = zeta(range, theta);
	zipf->eta = (1.0 - pow(2.0 / zipf->range, 1.0 - zipf->theta)) /
		    (1.0 - zeta(2, theta) / zipf->zetan);
	zipf->val1_limit = 1.0 + pow(0.5, zipf->theta);

	return zipf;
}

void
spdk_zipf_free(struct spdk_zipf **zipfp)
{
	assert(zipfp != NULL);
	free(*zipfp);
	*zipfp = NULL;
}

uint64_t
spdk_zipf_generate(struct spdk_zipf *zipf)
{
	double randu, randz;
	uint64_t val;

	randu = (double)rand_r(&zipf->seed) / RAND_MAX;
	randz = randu * zipf->zetan;

	if (randz < 1.0) {
		return 0;
	} else if (randz < zipf->val1_limit) {
		return 1;
	} else {
		val = zipf->range * pow(zipf->eta * (randu - 1.0) + 1.0, zipf->alpha);
		return val % zipf->range;
	}
}
