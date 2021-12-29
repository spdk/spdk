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

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/histogram_data.h"

/*
 * This applications is a simple test app used to test the performance of
 *  tallying datapoints with struct spdk_histogram_data.  It can be used
 *  to measure the effect of changes to the spdk_histogram_data implementation.
 *
 * There are no command line parameters currently - it just tallies
 *  datapoints for 10 seconds in a default-sized histogram structure and
 *  then prints out the number of tallies performed.
 */

static void
usage(const char *prog)
{
	printf("usage: %s\n", prog);
	printf("Options:\n");
}

int
main(int argc, char **argv)
{
	struct spdk_histogram_data *h;
	struct spdk_env_opts opts;
	uint64_t tsc[128], t, end_tsc, count;
	uint32_t i;
	int ch;
	int rc = 0;

	while ((ch = getopt(argc, argv, "")) != -1) {
		switch (ch) {
		default:
			usage(argv[0]);
			return 1;
		}
	}

	spdk_env_opts_init(&opts);
	if (spdk_env_init(&opts)) {
		printf("Err: Unable to initialize SPDK env\n");
		return 1;
	}

	for (i = 0; i < SPDK_COUNTOF(tsc); i++) {
		tsc[i] = spdk_get_ticks();
	}

	end_tsc = spdk_get_ticks() + (10 * spdk_get_ticks_hz());
	count = 0;
	h = spdk_histogram_data_alloc();

	while (true) {
		t = spdk_get_ticks();
		spdk_histogram_data_tally(h, t - tsc[count % 128]);
		count++;
		if (t > end_tsc) {
			break;
		}
	}

	printf("count = %ju\n", count);
	spdk_histogram_data_free(h);

	spdk_env_fini();
	return rc;
}
