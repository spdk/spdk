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

#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/uuid.h"

struct spdk_bs_dev *g_bs_dev;
const char *g_bdev_name;

static void
spdk_bsdump_done(void *arg, int bserrno)
{
	spdk_app_stop(0);
}

static void
bsdump_print_xattr(FILE *fp, const char *bstype, const char *name, const void *value,
		   size_t value_len)
{
	if (strncmp(bstype, "BLOBFS", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		if (strcmp(name, "name") == 0) {
			fprintf(fp, "%s", (char *)value);
		} else if (strcmp(name, "length") == 0 && value_len == sizeof(uint64_t)) {
			uint64_t length;

			memcpy(&length, value, sizeof(length));
			fprintf(fp, "%" PRIu64, length);
		} else {
			fprintf(fp, "?");
		}
	} else if (strncmp(bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH) == 0) {
		if (strcmp(name, "name") == 0) {
			fprintf(fp, "%s", (char *)value);
		} else if (strcmp(name, "uuid") == 0 && value_len == sizeof(struct spdk_uuid)) {
			char uuid[SPDK_UUID_STRING_LEN];

			spdk_uuid_fmt_lower(uuid, sizeof(uuid), (struct spdk_uuid *)value);
			fprintf(fp, "%s", uuid);
		} else {
			fprintf(fp, "?");
		}
	} else {
		fprintf(fp, "?");
	}
}

static void
spdk_bsdump_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;

	bdev = spdk_bdev_get_by_name(g_bdev_name);

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name);
		spdk_app_stop(-1);
		return;
	}

	bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);

	spdk_bs_dump(bs_dev, stdout, bsdump_print_xattr, spdk_bsdump_done, NULL);
}

static void
bsdump_usage(void)
{
	printf(" -b <bdev name> [Required]\n");
}

static void
bsdump_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_bdev_name = arg;
		break;
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	spdk_app_opts_init(&opts);
	opts.name = "bsdump";
	opts.config_file = "bsdump.conf";
	opts.reactor_mask = "0x1";
	opts.mem_size = 512;
	opts.shutdown_cb = NULL;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", bsdump_parse_arg, bsdump_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_bdev_name == NULL) {
		SPDK_ERRLOG("bdev name not specified - use -b <bdev name>\n");
		spdk_app_usage();
		bsdump_usage();
		exit(1);
	}

	rc = spdk_app_start(&opts, spdk_bsdump_run, NULL, NULL);
	spdk_app_fini();

	return rc;
}
