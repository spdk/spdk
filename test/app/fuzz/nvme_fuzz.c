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

#define DEFAULT_RUNTIME_US 30000000 /* 30 seconds */
#define S_TO_US 1000000
#define IO_TIMEOUT_US (S_TO_US * 5)

struct spdk_bdev_desc *g_spdk_nvme_bdev;
struct spdk_io_channel *g_io_ch;
struct spdk_poller *g_runtime_poller;
struct spdk_poller *g_timeout_poller;
struct spdk_nvme_cmd *g_cmd;
uint64_t g_io_counter;
uint64_t g_prev_io_counter;
uint32_t g_runtime;
uint32_t g_outstanding_io;
bool g_run;


static void
print_nvme_cmd(struct spdk_nvme_cmd *cmd)
{
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

		spdk_put_io_channel(g_io_ch);
		spdk_bdev_close(g_spdk_nvme_bdev);

		spdk_app_stop(0);

	}
}

static void
bdev_reset_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	g_run = 0;
	g_outstanding_io = 0;
	free(g_cmd);
	check_for_exit();
}

static int
check_timeout(void *ctx)
{
	if (g_io_counter == g_prev_io_counter) {
		if (g_runtime_poller != NULL) {
			spdk_poller_unregister(&g_runtime_poller);
		}

		if (g_timeout_poller != NULL) {
			spdk_poller_unregister(&g_timeout_poller);
		}

		SPDK_ERRLOG("The following I/O (I/O num %lu) caused the device to hang.\n", g_io_counter);
		print_nvme_cmd(g_cmd);

		SPDK_ERRLOG("Exiting early\n");
		if (spdk_bdev_reset(g_spdk_nvme_bdev, g_io_ch, bdev_reset_cb, NULL)) {
			SPDK_ERRLOG("Unable to reset the bdev. You will most likely have to manually kill this process\n");
			g_run = 0;
			g_outstanding_io = 0;
			free(g_cmd);
			check_for_exit();
		}
	} else {
		g_prev_io_counter = g_io_counter;
	}
	return 0;
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
	++g_io_counter;
	if (spdk_unlikely(success)) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
			SPDK_NOTICELOG("The following admin command (i/o num %lu) completed successfully\n", g_io_counter);
			break;
		case SPDK_BDEV_IO_TYPE_NVME_IO:
			SPDK_NOTICELOG("The following io command (i/o num %lu) completed successfully\n", g_io_counter);
			break;
		default:
			SPDK_NOTICELOG("A command of unknown type (i/o num %lu) completed successfully\n", g_io_counter);
			break;
		}

		print_nvme_cmd(g_cmd);
	}
	--g_outstanding_io;

	spdk_bdev_free_io(bdev_io);
	free(g_cmd);

	submit_next_io();
	check_for_exit();
}

static void
submit_next_io(void)
{
	int rc;

	if (g_run) {

		g_cmd = prep_nvme_cmd();

		if (g_io_counter % 2 == 1) {
			if ((rc = spdk_bdev_nvme_io_passthru(g_spdk_nvme_bdev, g_io_ch, g_cmd, NULL, 0, nvme_fuzz_cpl_cb,
							     NULL))) {
				SPDK_ERRLOG("Unable to submit passthrough command with %lu total io and %u outstanding io and rc %d\n",
					    g_io_counter, g_outstanding_io, rc);
				free(g_cmd);
				g_run = 0;
				return;
			}
		} else {
			if ((rc = spdk_bdev_nvme_admin_passthru(g_spdk_nvme_bdev, g_io_ch, g_cmd, NULL, 0, nvme_fuzz_cpl_cb,
								NULL))) {
				SPDK_ERRLOG("Unable to submit passthrough command with %lu total io and %u outstanding io and rc %d\n",
					    g_io_counter, g_outstanding_io, rc);
				free(g_cmd);
				g_run = 0;
				return;
			}
		}
		g_outstanding_io++;
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

	if (g_runtime) {
		g_runtime_poller = spdk_poller_register(finish_io, NULL, g_runtime * S_TO_US);
	} else {
		g_runtime_poller = spdk_poller_register(finish_io, NULL, DEFAULT_RUNTIME_US);
	}

	seed_random();

	g_timeout_poller = spdk_poller_register(check_timeout, NULL, IO_TIMEOUT_US);
	submit_next_io();
}

static void
nvme_fuzz_parse(int ch, char *arg)
{
	switch (ch) {
	case 't':
		g_runtime = atoi(arg);
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
	g_runtime = 0;

	rc = spdk_app_parse_args(argc, argv, &opts, "t:", NULL, nvme_fuzz_parse, nvme_fuzz_usage);

	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		SPDK_ERRLOG("Failed to parse the arguments for the nvme_fuzz application.\n");
		return -1;
	}

	g_run = true;
	g_outstanding_io = 0;
	g_prev_io_counter = 0;
	g_io_counter = 0;
	rc = spdk_app_start(&opts, begin_fuzz, NULL, NULL);

	SPDK_NOTICELOG("Shutting down the fuzz application\n");
	spdk_app_fini();
	return rc;
}
