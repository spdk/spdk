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
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/nvme_spec.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"

#define TEST_QUEUE_DEPTH 32
#define DEFAULT_TIMEOUT_US 30000000 /* 30 seconds */
#define S_TO_US 1000000

struct spdk_bdev_desc *g_spdk_nvme_bdev;
struct spdk_io_channel *g_io_ch;
struct spdk_poller *g_timeout_poller;
uint64_t g_io_counter;
uint32_t g_timeout;
uint32_t g_outstanding_io;
bool g_run;


static int
finish_io(void *ctx)
{
	SPDK_NOTICELOG("Turning off I/O submission\n");
	g_run = 0;

	return 0;
}

static void
check_for_exit(void)
{
	if (g_run == 0 && g_outstanding_io == 0) {

		if (g_timeout_poller != NULL) {
			spdk_poller_unregister(&g_timeout_poller);
		}
		spdk_put_io_channel(g_io_ch);
		spdk_bdev_close(g_spdk_nvme_bdev);

		spdk_app_stop(0);

	}
}

static void
seed_random(void)
{
	time_t seed_time;
	seed_time = time(0);
	SPDK_NOTICELOG("Seed value for this run %lu\n", seed_time);
	srand(time(0));
}

static uint8_t
random_character(void)
{
	return rand() % UINT8_MAX;
}

static struct spdk_nvme_cmd *
prep_nvme_cmd(void)
{
	size_t cmd_size = sizeof(struct spdk_nvme_cmd);
	struct spdk_nvme_cmd *cmd = malloc(cmd_size);
	char *character_repr = (char *)cmd;
	size_t i;

	for (i = 0; i < cmd_size; i++) {
		character_repr[i] = random_character();
	}

	return cmd;
}

static void submit_next_io(void);

static void
nvme_fuzz_cpl_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvme_cmd *cmd = cb_arg;

	++g_io_counter;
	if (spdk_unlikely(success)) {
		SPDK_NOTICELOG("The following bdevio (i/o num %lu) completed successfully\n", g_io_counter);
		SPDK_NOTICELOG("opc %u\n", cmd->opc);
		SPDK_NOTICELOG("fuse %u\n", cmd->fuse);
		SPDK_NOTICELOG("rsvd1 %u\n", cmd->rsvd1);
		SPDK_NOTICELOG("psdt %u\n", cmd->psdt);
		SPDK_NOTICELOG("cid %u\n", cmd->cid);
		SPDK_NOTICELOG("nsid %u\n", cmd->nsid);
		SPDK_NOTICELOG("rsvd2 %u\n", cmd->rsvd2);
		SPDK_NOTICELOG("rsvd3 %u\n", cmd->rsvd3);
		SPDK_NOTICELOG("mptr %lu\n", cmd->mptr);
		SPDK_NOTICELOG("cdw10 %u\n", cmd->cdw10);
		SPDK_NOTICELOG("cdw11 %u\n", cmd->cdw11);
		SPDK_NOTICELOG("cdw12 %u\n", cmd->cdw12);
		SPDK_NOTICELOG("cdw13 %u\n", cmd->cdw13);
		SPDK_NOTICELOG("cdw14 %u\n", cmd->cdw14);
		SPDK_NOTICELOG("cdw15 %u\n", cmd->cdw15);
	}
	--g_outstanding_io;

	spdk_bdev_free_io(bdev_io);
	free(cmd);

	submit_next_io();
	check_for_exit();
}

static void
submit_next_io(void)
{
	struct spdk_nvme_cmd *cmd;
	int rc;

	if (g_run) {
		/* Purposefully avoid flushes, since those are always successful regardless of the rest of the command */
		do {
			cmd = prep_nvme_cmd();
		} while (cmd->opc == 0);

		if ((rc = spdk_bdev_nvme_io_passthru(g_spdk_nvme_bdev, g_io_ch, cmd, NULL, 0, nvme_fuzz_cpl_cb,
						     cmd))) {
			SPDK_ERRLOG("Unable to submit passthrough command with %lu total io and %u outstanding io and rc %d\n",
				    g_io_counter, g_outstanding_io, rc);
			free(cmd);
			g_run = 0;
			return;
		}

		g_outstanding_io++;
	}
}

static void
start_performing_io(void)
{
	int i;

	for (i = 0; i < TEST_QUEUE_DEPTH; i++) {
		submit_next_io();
		check_for_exit();
	}
}

static void
begin_fuzz(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;
	int rc;

	bdev = spdk_bdev_first();

	while (bdev != NULL && strcmp(spdk_bdev_get_product_name(bdev), "NVMe disk")) {
		bdev = spdk_bdev_next(bdev);
	}

	if (bdev == NULL) {
		SPDK_ERRLOG("Unable to locate an NVMe bdev\n");
		spdk_app_stop(-1);
		return;
	} else {
		rc = spdk_bdev_open(bdev, true, NULL, NULL, &g_spdk_nvme_bdev);
		if (rc) {
			SPDK_ERRLOG("Failed to open the NVMe bdev\n");
			spdk_app_stop(-1);
			return;
		}
	}
	SPDK_NOTICELOG("bdev name %s\n", spdk_bdev_get_name(bdev));

	g_io_ch = spdk_bdev_get_io_channel(g_spdk_nvme_bdev);
	if (g_io_ch == NULL) {
		SPDK_ERRLOG("Failed to open a channel to the NVMe bdev\n");
		spdk_bdev_close(g_spdk_nvme_bdev);
		spdk_app_stop(-1);
		return;
	}

	if (g_timeout) {
		g_timeout_poller = spdk_poller_register(finish_io, NULL, g_timeout * S_TO_US);
	} else {
		g_timeout_poller = spdk_poller_register(finish_io, NULL, DEFAULT_TIMEOUT_US);
	}

	seed_random();
	start_performing_io();
}

static void
nvme_fuzz_parse(int ch, char *arg)
{
	switch (ch) {
	case 't':
		g_timeout = atoi(arg);
		break;
	}
}

static void
nvme_fuzz_usage(void)
{
	printf(" -t <integer>              time in second to run the fuzz test.\n");
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts);
	opts.name = "nvme_fuzz";
	opts.mem_size = 2048;
	g_timeout = 0;

	rc = spdk_app_parse_args(argc, argv, &opts, "t:", NULL, nvme_fuzz_parse, nvme_fuzz_usage);

	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		SPDK_ERRLOG("Failed to parse the arguments for the nvme_fuzz application.\n");
		return -1;
	}

	g_run = true;
	g_outstanding_io = 0;
	g_io_counter = 0;
	rc = spdk_app_start(&opts, begin_fuzz, NULL, NULL);

	SPDK_NOTICELOG("Shutting down the fuzz application\n");
	spdk_app_fini();
	return rc;
}
