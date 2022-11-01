/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
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
