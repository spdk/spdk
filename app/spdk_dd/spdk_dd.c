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

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/vmd.h"

struct spdk_dd_opts {
	char		*input_file;
	char		*output_file;
	char		*input_bdev;
	char		*output_bdev;
	uint64_t	input_offset;
	uint64_t	output_offset;
	int64_t		io_unit_size;
	int64_t		io_unit_count;
	uint32_t	queue_depth;
};

static struct spdk_dd_opts g_opts = {
	.io_unit_size = 4096,
	.queue_depth = 2,
};

enum dd_cmdline_opts {
	DD_OPTION_IF = 0x1000,
	DD_OPTION_OF,
	DD_OPTION_IB,
	DD_OPTION_OB,
	DD_OPTION_SKIP,
	DD_OPTION_SEEK,
	DD_OPTION_BS,
	DD_OPTION_QD,
	DD_OPTION_COUNT,
};

static struct option g_cmdline_opts[] = {
	{
		.name = "if",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IF,
	},
	{
		.name = "of",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OF,
	},
	{
		.name = "ib",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IB,
	},
	{
		.name = "ob",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OB,
	},
	{
		.name = "skip",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SKIP,
	},
	{
		.name = "seek",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SEEK,
	},
	{
		.name = "bs",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_BS,
	},
	{
		.name = "qd",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_QD,
	},
	{
		.name = "count",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_COUNT,
	},
	{
		.name = NULL
	}
};

static void
usage(void)
{
	printf("[--------- DD Options ---------]\n");
	printf(" --if Input file. Must specify either --if or --ib.\n");
	printf(" --ib Input bdev. Must specifier either --if or --ib\n");
	printf(" --of Output file. Must specify either --of or --ob.\n");
	printf(" --ob Output bdev. Must specify either --of or --ob.\n");
	printf(" --bs I/O unit size (default: %" PRId64 ")\n", g_opts.io_unit_size);
	printf(" --qd Queue depth (default: %d)\n", g_opts.queue_depth);
	printf(" --count I/O unit count. The number of I/O units to copy. (default: all)\n");
	printf(" --skip Skip this many I/O units at start of input. (default: 0)\n");
	printf(" --seek Skip this many I/O units at start of output. (default: 0)\n");
}

static int
parse_args(int argc, char *argv)
{
	switch (argc) {
	case DD_OPTION_IF:
		g_opts.input_file = strdup(argv);
		break;
	case DD_OPTION_OF:
		g_opts.output_file = strdup(argv);
		break;
	case DD_OPTION_IB:
		g_opts.input_bdev = strdup(argv);
		break;
	case DD_OPTION_OB:
		g_opts.output_bdev = strdup(argv);
		break;
	case DD_OPTION_SKIP:
		g_opts.input_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_SEEK:
		g_opts.output_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_BS:
		g_opts.io_unit_size = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_QD:
		g_opts.queue_depth = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_COUNT:
		g_opts.io_unit_count = spdk_strtol(optarg, 10);
		break;
	default:
		usage();
		return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 1;

	spdk_app_opts_init(&opts);
	opts.reactor_mask = "0x1";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", g_cmdline_opts, parse_args,
				      usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		goto end;
	}

	if (g_opts.input_file != NULL && g_opts.input_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --if or --ib, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file != NULL && g_opts.output_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --of or --ob, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.input_file == NULL && g_opts.input_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --if or --ib\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file == NULL && g_opts.output_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --of or --ob\n");
		rc = EINVAL;
		goto end;
	}

end:
	return rc;
}
