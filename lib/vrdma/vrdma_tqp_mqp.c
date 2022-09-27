/*
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

#include "config.h"
#include "mlnx_snap_spdk.h"
#include "mlnx_snap_pci_manager.h"
#include "spdk_emu_mgr.h"
#include "spdk_io_mgr.h"
#include "nvme_emu_log.h"
#include "spdk_ext_io/spdk_ext_io.h"
#include "effdma/effdma_domain.h"
#ifdef HAVE_SPDK_RPC_FINISH
#include <spdk/init.h>
#endif

int mlnx_snap_spdk_init()
{
    int rc;
    extern int nvme_spdk_subsystem;

    nvme_spdk_subsystem = 1;
    rc = nvme_init_logger();
    return rc;
}

void mlnx_snap_spdk_cleanup()
{
    nvme_close_logger();
}

int mlnx_snap_pci_check_hotunplug(struct snap_context *sctx, struct ibv_device *ibdev)
{
    struct snap_pci **pfs;
    int num_pfs, i;
    struct spdk_emu_ctx *ctx;
    int ret = 0;

    pfs = calloc(sctx->virtio_blk_pfs.max_pfs, sizeof(*pfs));
    if (!pfs)
        return -ENOMEM;

    num_pfs = snap_get_pf_list(sctx, SNAP_VIRTIO_BLK, pfs);
    for (i = 0; i < num_pfs; i++) {
       if (pfs[i]->pci_hotunplug_state == snap_pci_needs_controller_hotunplug) {
           ctx = spdk_emu_hotunplug_create_controller(pfs[i], ibdev);
           if (!ctx) {
               nvmx_warn("Failed to create controller for hotunplug virtio_blk pf %d", i);
               ret = -EINVAL;
           }
       }
    }

    free(pfs);
    return ret;
}

int mlnx_snap_spdk_start()
{
    struct ibv_device **list;
    int i, dev_count;
    struct snap_context *sctx;

    if (mlnx_snap_pci_manager_init()) {
        nvmx_error("Failed to init emulation managers list");
        goto err;
    }

    if (spdk_ext_io_init()) {
        nvmx_error("Failed to init SPDK EXT_IO path");
        goto clear_pci;
    }

    if (spdk_io_mgr_init()) {
        nvmx_error("Failed to init SPDK IO manager");
        goto clear_zcopy;
    }

    if (spdk_emu_init()) {
        nvmx_error("Failed to init SPDK EMU manager");
        goto clear_mgr;
    }

    list = ibv_get_device_list(&dev_count);
    if (!list) {
        nvmx_error("failed to open IB device list");
        goto err;
    }

    for (i = 0; i < dev_count; i++) {
        sctx = mlnx_snap_get_snap_context(ibv_get_device_name(list[i]));
        if (sctx && mlnx_snap_pci_check_hotunplug(sctx, list[i])) {
            nvmx_warn("Failed to hotunplug one or more emulation functions");
        }
    }

    ibv_free_device_list(list);

    return 0;

clear_mgr:
    spdk_io_mgr_clear();
clear_zcopy:
    spdk_ext_io_clear();
clear_pci:
    mlnx_snap_pci_manager_clear();
err:
    return -1;
}

static volatile int spdk_emu_list_num_deleting = 0;

static void spdk_emu_ctx_fini_cb_app_stop(void *arg)
{
    void (*fini_cb)() = arg;

    pthread_mutex_lock(&spdk_emu_list_lock);
    spdk_emu_list_num_deleting--;
    pthread_mutex_unlock(&spdk_emu_list_lock);

    /* stop app only after all controllers are destroyed properly */
    if (spdk_emu_list_num_deleting == 0) {
        spdk_emu_clear();
        spdk_io_mgr_clear();
        spdk_ext_io_clear();
        mlnx_snap_pci_manager_clear();
        fini_cb();
    }
}

void mlnx_snap_spdk_stop(void (*fini_cb)())
{
    struct spdk_emu_ctx *ctx;
    int count = 0;

#ifdef HAVE_SPDK_RPC_FINISH
    /* Stop RPC at most earlier stage, we want to prevent receiving
     * RPC requests on application tear down.
     */
    spdk_emu_rpc_finished = true;
    spdk_rpc_finish();
#endif

    pthread_mutex_lock(&spdk_emu_list_lock);
    while ((ctx = LIST_FIRST(&spdk_emu_list)) != NULL) {
        LIST_REMOVE(ctx, entry);
        spdk_emu_list_num_deleting++;
        spdk_emu_ctx_destroy(ctx, spdk_emu_ctx_fini_cb_app_stop, fini_cb);
        count++;
    }
    pthread_mutex_unlock(&spdk_emu_list_lock);

    if (count == 0) {
        spdk_emu_clear();
        spdk_io_mgr_clear();
        spdk_ext_io_clear();
        effdma_domain_clear();
        mlnx_snap_pci_manager_clear();
        fini_cb();
    }
}
