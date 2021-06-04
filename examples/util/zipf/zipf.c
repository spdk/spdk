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

#include "spdk/stdinc.h"
#include "spdk/zipf.h"
#include "spdk/histogram_data.h"
#include "spdk/string.h"

static void
usage(const char *prog)
{
	printf("usage: %s <theta> <range> <count>\n", prog);
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	char range[64];

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far * 100 / total;
	snprintf(range, sizeof(range), "[%ju, %ju)", start, end);
	printf("%24s: %9.4f%%  (%9ju)\n", range, so_far_pct, count);
}

int
main(int argc, char **argv)
{
	struct spdk_zipf *zipf;
	struct spdk_histogram_data *h;
	float theta;
	int range, count, i;

	if (argc < 4) {
		usage(argv[0]);
		return 1;
	}

	theta = atof(argv[1]);
	range = spdk_strtol(argv[2], 10);
	count = spdk_strtol(argv[3], 10);

	if (range <= 0 || count <= 0) {
		printf("range and count must be positive integers\n");
		usage(argv[0]);
		return 1;
	}

	zipf = spdk_zipf_create(range, theta, time(NULL));
	h = spdk_histogram_data_alloc();
	if (zipf == NULL || h == NULL) {
		spdk_zipf_free(&zipf);
		spdk_histogram_data_free(h);
		printf("out of resource\n");
		return 1;
	}

	for (i = 0; i < count; i++) {
		spdk_histogram_data_tally(h, spdk_zipf_generate(zipf));
	}

	spdk_histogram_data_iterate(h, print_bucket, NULL);
	spdk_histogram_data_free(h);
	spdk_zipf_free(&zipf);

	return 0;
}
