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
#include <infiniband/verbs.h>
#include "spdk/config.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/log.h"
#include "snap.h"
#include "snap_vrdma.h"

struct vrdma_snap_emu_manager {
    struct snap_context *sctx;
    struct ibv_context *ibctx;
    struct ibv_device *ibdev;
    LIST_ENTRY(vrdma_snap_emu_manager) entry;
};

LIST_HEAD(, vrdma_snap_emu_manager) vrdma_snap_emu_manager_list =
                              LIST_HEAD_INITIALIZER(vrdma_snap_emu_manager_list);

struct ibv_device *spdk_vrdma_snap_get_ibv_device(const char *vrdma_dev)
{
    struct vrdma_snap_emu_manager *emu_manager;
    struct ibv_device *ibdev = NULL;

    LIST_FOREACH(emu_manager, &vrdma_snap_emu_manager_list, entry) {
        if (!strncmp(vrdma_dev, ibv_get_device_name(emu_manager->ibdev), 16)) {
            ibdev = emu_manager->ibdev;
            break;
        }
    }

    return ibdev;
}

struct ibv_context *spdk_vrdma_snap_get_ibv_context(const char *vrdma_dev)
{
    struct vrdma_snap_emu_manager *emu_manager;
    struct ibv_context *ibctx = NULL;

    LIST_FOREACH(emu_manager, &vrdma_snap_emu_manager_list, entry) {
        if (!strncmp(vrdma_dev, ibv_get_device_name(emu_manager->ibdev), 16)) {
            ibctx = emu_manager->ibctx;
            break;
        }
    }

    return ibctx;
}

struct snap_context *spdk_vrdma_snap_get_snap_context(const char *vrdma_dev)
{
    struct vrdma_snap_emu_manager *emu_manager;
    struct snap_context *sctx = NULL;

    LIST_FOREACH(emu_manager, &vrdma_snap_emu_manager_list, entry) {
        if (!strncmp(vrdma_dev, ibv_get_device_name(emu_manager->ibdev), 16)) {
            sctx = emu_manager->sctx;
            break;
        }
    }

    return sctx;
}

struct snap_pci **spdk_vrdma_snap_get_snap_pci_list(const char *vrdma_dev,
                                              int *spci_list_sz)
{
    struct snap_context *sctx;
    struct snap_pci **pf_list;
    const struct snap_pfs_ctx *pfs;

    sctx = spdk_vrdma_snap_get_snap_context(vrdma_dev);
    if (!sctx) {
        SPDK_WARNLOG("Cannot find snap context on %s", vrdma_dev);
        goto err;
    }

    pfs = &sctx->vrdma_pfs;
    if (!pfs || !pfs->max_pfs) {
        SPDK_WARNLOG("No PFs of type VRDMA");
        goto err;
    }

    pf_list = calloc(pfs->max_pfs, sizeof(*pf_list));
    if (!pf_list) {
        SPDK_ERRLOG("Cannot allocate PF list");
        goto err;
    }
    *spci_list_sz = snap_get_pf_list(sctx, SNAP_VRDMA, pf_list);

    return pf_list;

err:
    *spci_list_sz = 0;
    return NULL;
}

struct snap_pci *spdk_vrdma_snap_get_snap_pci(const char *vrdma_dev, int pf_index)
{
    struct snap_pci **pf_list;
    struct snap_pci *pci_func = NULL;
    int pf_list_sz;

    pf_list = spdk_vrdma_snap_get_snap_pci_list(vrdma_dev, &pf_list_sz);
    if (!pf_list)
        return NULL;

    if (pf_index >= pf_list_sz) {
        SPDK_ERRLOG("PF %d exceeds limit (%d)", pf_index, pf_list_sz);
    } else {
        pci_func = (struct snap_pci *)&pf_list[pf_index];
    }
    free(pf_list);

    if (!pci_func)
            SPDK_ERRLOG("pci_func cannot be found on %s", vrdma_dev);

    return pci_func;
}

void spdk_vrdma_snap_pci_mgr_clear(void)
{
    struct vrdma_snap_emu_manager *emu_manager;

    while ((emu_manager = LIST_FIRST(&vrdma_snap_emu_manager_list)) != NULL) {
        LIST_REMOVE(emu_manager, entry);
        if (emu_manager->sctx)
            snap_close(emu_manager->sctx);
        else
            ibv_close_device(emu_manager->ibctx);
        free(emu_manager);
    }
}

int spdk_vrdma_snap_pci_mgr_init(void)
{
    struct ibv_device **list;
    struct vrdma_snap_emu_manager *emu_manager;
    struct snap_context *sctx;
    struct ibv_context *ibctx;
    int i, dev_count;
    bool found_emu_managers = false;

    list = ibv_get_device_list(&dev_count);
    if (!list) {
        SPDK_ERRLOG("failed to open IB device list");
        goto err;
    }

    /* Verify RDMA device exist in ibv list */
    for (i = 0; i < dev_count; i++) {
        emu_manager = calloc(1, sizeof(*emu_manager));
        if (!emu_manager)
            goto clear_emu_manager_list;

        sctx = snap_open(list[i]);
        if (sctx) {
            found_emu_managers = true;
            ibctx = sctx->context;
            snap_vrdma_pci_functions_cleanup(sctx);
        } else {
            ibctx = ibv_open_device(list[i]);
            if (!ibctx) {
                free(emu_manager);
                goto clear_emu_manager_list;
            }
        }

        emu_manager->sctx = sctx;
        emu_manager->ibctx = ibctx;
        emu_manager->ibdev = list[i];

        LIST_INSERT_HEAD(&vrdma_snap_emu_manager_list, emu_manager, entry);
    }
    if (!found_emu_managers) {
        SPDK_ERRLOG("No emulation managers detected");
        goto clear_emu_manager_list;
    }

    ibv_free_device_list(list);
    return 0;

clear_emu_manager_list:
    spdk_vrdma_snap_pci_mgr_clear();
    ibv_free_device_list(list);
err:
    return -EINVAL;
}
