/*
 *   Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/vrdma_snap.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_admq.h"

int spdk_vrdma_snap_start(void)
{
    SPDK_NOTICELOG("lizh spdk_vrdma_snap_start...start");
    if (spdk_vrdma_adminq_resource_init()) {
        SPDK_ERRLOG("Failed to init admin-queue resource");
        goto err;
    }
    if (spdk_vrdma_snap_pci_mgr_init()) {
        SPDK_ERRLOG("Failed to init emulation managers list");
        goto err;
    }
    if (spdk_io_mgr_init()) {
        SPDK_ERRLOG("Failed to init SPDK IO manager");
        goto clear_pci;
    }

    return 0;
clear_pci:
    spdk_vrdma_snap_pci_mgr_clear();
err:
    return -1;
}

static volatile int spdk_emu_list_num_deleting = 0;

void spdk_vrdma_snap_stop(void (*fini_cb)(void))
{
    struct spdk_emu_ctx *ctx;
    int count = 0;

    pthread_mutex_lock(&spdk_emu_list_lock);
    while ((ctx = LIST_FIRST(&spdk_emu_list)) != NULL) {
        LIST_REMOVE(ctx, entry);
        spdk_emu_list_num_deleting++;
        spdk_emu_ctx_destroy(ctx);
        count++;
    }
    pthread_mutex_unlock(&spdk_emu_list_lock);

    if (count == 0) {
        spdk_io_mgr_clear();
        spdk_vrdma_snap_pci_mgr_clear();
        spdk_vrdma_adminq_resource_destory();
        fini_cb();
    }
}
