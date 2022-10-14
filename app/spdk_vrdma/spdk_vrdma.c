/*-
 *   BSD LICENSE
 *
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/net.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/vrdma.h"

static struct spdk_thread *g_vrdma_app_thread;
static int g_start_mqpn;
static struct spdk_vrdma_ctx g_vrdma_ctx;

#define MAX_START_MQP_NUM 0x40000

static void stop_done_cb(void)
{
    spdk_app_stop(0);
}

static void spdk_vrdma_app_stop(void *arg)
{
    spdk_vrdma_ctx_stop(stop_done_cb);
}

static void spdk_vrdma_signal_handler(int dummy)
{
    spdk_thread_send_msg(g_vrdma_app_thread, spdk_vrdma_app_stop, NULL);
}

static void spdk_vrdma_app_start(void *arg)
{
    struct sigaction act;

    SPDK_ERRLOG("lizh spdk_vrdma_app_start...start\n");
    /*
     * Set signal handler to allow stop on Ctrl+C.
     */
    g_vrdma_app_thread = spdk_get_thread();
    if (!g_vrdma_app_thread) {
        SPDK_ERRLOG("Failed to get SPDK thread\n");
        goto err;
    }
    memset(&g_vrdma_ctx, 0, sizeof(struct spdk_vrdma_ctx));
    if (spdk_vrdma_ctx_start(&g_vrdma_ctx)) {
        SPDK_ERRLOG("Failed to start VRDMA_SNAP\n");
        goto err;
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = spdk_vrdma_signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    SPDK_NOTICELOG("VRDMA_SNAP started successfully\n");
    return;

err:
    spdk_app_stop(-1);
}

static void
vrdma_usage(void)
{
	fprintf(stderr, " -q <integer>              remote_mqp start mlnx qp number.\n");
}

static int
vrdma_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'q':
		g_start_mqpn = spdk_strtol(arg, 10);
		if (g_start_mqpn < 0 || g_start_mqpn > MAX_START_MQP_NUM) {
			fprintf(stderr,
			"You must supply a positive mqp number less than 4096.\n");
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int main(int argc, char **argv)
{
    struct spdk_app_opts opts = {0};
    int rc;

    /* Set default values in opts structure. */
    spdk_app_opts_init(&opts);
    opts.name = "spdk_vrdma";

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "q", NULL,
    	vrdma_parse_arg, vrdma_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
	    fprintf(stderr, "Unable to parse the application arguments.\n");
        exit(rc);
    }

    rc = spdk_app_start(&opts, spdk_vrdma_app_start, NULL);
    if (rc) {
        SPDK_ERRLOG("ERROR starting application\n");
    }

    /* Gracefully close out all of the SPDK subsystems. */
    SPDK_NOTICELOG("Exiting...\n");
    spdk_app_fini();
    return rc;
}
