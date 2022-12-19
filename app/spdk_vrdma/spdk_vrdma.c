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
#include "spdk/vrdma_controller.h"

static struct spdk_thread *g_vrdma_app_thread;
static struct spdk_vrdma_ctx g_vrdma_ctx;
static struct vrdma_dev_mac g_dev_mac;

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

    SPDK_NOTICELOG("lizh spdk_vrdma_app_start...start\n");
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
	fprintf(stderr, " -v --pci_mac   [pci_number]:[mac], such as [af:00.2]:[11:22:33:44:55:66]\n");
}

static int
vrdma_parse_dev_mac(char *arg)
{
    char vrdma_dev[MAX_VRDMA_DEV_LEN];
    char *str, *next, *pci_str, *mac_str;
    char mac[6];
    uint64_t temp_mac;
    int i;

    SPDK_NOTICELOG("lizh vrdma_parse_dev_mac arg %s mac 0x%lx \n", arg, g_dev_mac.mac);
    snprintf(vrdma_dev, MAX_VRDMA_DEV_LEN, "%s", arg);
    next = vrdma_dev;

	if (next[0] == '[') {
		str = strchr(next, ']');
        pci_str = &next[1];
        *str = '\0';
        memcpy(g_dev_mac.pci_number, pci_str, VRDMA_PCI_NAME_MAXLEN);
        SPDK_NOTICELOG("lizh vrdma_parse_dev_mac pci_number %s \n", g_dev_mac.pci_number);
        next = str + 2;
    } else {
        SPDK_NOTICELOG("lizh vrdma_parse_dev_mac next[0] 0x%x \n", next[0]);
        return -EINVAL;
    }
    if (next[0] == '[') {
        mac_str = &next[1];
        for (i = 0; i < 6; i++) {
            if ((i < 5 && mac_str[2] != ':') ||
                (i == 5 && mac_str[2] != ']') ) {
                SPDK_NOTICELOG("lizh vrdma_parse_dev_mac mac_str[2] 0x%x\n", mac_str[2]);
                return -EINVAL;
            }
            if (i < 5)
                str = strchr(mac_str, ':');
            else
                str = strchr(mac_str, ']');
            *str = '\0';
            mac[i] = spdk_strtol(mac_str, 16);
            SPDK_NOTICELOG("lizh vrdma_parse_dev_mac mac[%d] 0x%x \n", i, mac[i]);
            temp_mac = mac[i] & 0xFF;
            g_dev_mac.mac |= temp_mac << ((5-i) * 8);
            mac_str += 3;
        }
        SPDK_NOTICELOG("lizh vrdma_parse_dev_mac g_dev_mac.mac 0x%lx \n", g_dev_mac.mac);
    } else {
        SPDK_NOTICELOG("lizh vrdma_parse_dev_mac next[0] 0x%x next[1] 0x%x\n", next[0], next[1]);
        return -EINVAL;
    }
    return 0;
}

static int
vrdma_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'v':
        SPDK_NOTICELOG("lizh vrdma_parse_arg pci_number %s mac 0x%lx \n", arg, g_dev_mac.mac);
        if (vrdma_parse_dev_mac(arg))
            return -EINVAL;
		vrdma_dev_mac_add(g_dev_mac.pci_number, g_dev_mac.mac);
        memset(&g_dev_mac, 0, sizeof(struct vrdma_dev_mac));
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
    memset(&g_dev_mac, 0, sizeof(struct vrdma_dev_mac));
    if ((rc = spdk_app_parse_args(argc, argv, &opts, "v:", NULL,
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
