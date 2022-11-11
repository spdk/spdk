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

#define __GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <infiniband/verbs.h>

#include "snap.h"
#include "snap_env.h"
#include "snap_vrdma.h"
#include "snap_vrdma_ctrl.h"

//#include "config.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/event.h"
#include <spdk/rpc.h>
#include <spdk/env.h>
#include <spdk/json.h>
#include <spdk/jsonrpc.h>
#include "spdk/vrdma.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"

static int spdk_emu_ctx_io_threads_create(struct spdk_emu_ctx *ctrl_ctx);
static void spdk_emu_ctrl_destroy(struct spdk_emu_ctx *ctx,
                                  void (*done_cb)(void *arg),
                                  void *done_cb_arg);

pthread_mutex_t spdk_emu_list_lock = PTHREAD_MUTEX_INITIALIZER;
struct spdk_emu_list_head spdk_emu_list =
                              LIST_HEAD_INITIALIZER(spdk_emu_list);

struct spdk_emu_controller_vrdma_create_attr {
    char emu_manager[MAX_VRDMA_DEV_LEN];
    int   pf_id;
    bool force_in_order;
    bool suspended;
};

struct spdk_emu_ctx *
spdk_emu_ctx_find_by_pci_id(const char *emu_manager, int pf_id)
{
    struct spdk_emu_ctx *ctx;

    LIST_FOREACH(ctx, &spdk_emu_list, entry) {
        /*lizh Just for test*/
        SPDK_NOTICELOG("lizh spdk_emu_ctx_find_by_pci_id...%s type %d id %d",
        ctx->emu_manager, ctx->spci->type, ctx->spci->id);
        if (strncmp(ctx->emu_manager, emu_manager,
                    SPDK_EMU_MANAGER_NAME_MAXLEN) ||
            ctx->spci->type != SNAP_VIRTIO_NET_PF)
        /*if (strncmp(ctx->emu_manager, emu_manager,
                    SPDK_EMU_MANAGER_NAME_MAXLEN) ||
            ctx->spci->type != SNAP_VRDMA_PF)*/
            continue;

        if (ctx->spci->id == pf_id)
            return ctx;
    }
    return NULL;
}

struct spdk_emu_ctx *spdk_emu_ctx_find_by_emu_name(const char *emu_name)
{
    struct spdk_emu_ctx *ctx;

    LIST_FOREACH(ctx, &spdk_emu_list, entry) {
        if (!strncmp(ctx->emu_name, emu_name, SPDK_EMU_NAME_MAXLEN))
            return ctx;
    }
    return NULL;
}

struct spdk_emu_ctx_ctrl_ops {
    const char *prefix;
    int (*progress)(void *ctrl); /*Admin queue progress*/
    void (*progress_mmio)(void *ctrl); /*ctrl bar event progress*/
    int (*progress_io)(void *ctrl);
    int (*progress_io_thread)(void *ctrl, int thread_id);
    /* stop accepting new requests and complete all outstaning
     * requests. The function is async */
    void (*suspend)(void *ctrl);
    /* true if controller has completed suspend */
    bool (*is_suspended)(void *ctrl);
    /* reverse of suspend */
    int (*resume)(void *ctrl);
};

static void spdk_emu_ctx_stop_pollers(struct spdk_emu_ctx *ctx);

static inline int spdk_emu_progress(void *arg)
{
    struct spdk_emu_ctx *ctx = arg;

    ctx->ctrl_ops->progress(ctx->ctrl);

    /* suspend must be initiated by us */
    if (ctx->should_stop && ctx->ctrl_ops->is_suspended(ctx->ctrl)) {
        ctx->should_stop = false;
        spdk_emu_ctx_stop_pollers(ctx);
    }
    return 0;
}

static inline int spdk_emu_progress_mmio(void *arg)
{
    struct spdk_emu_ctx *ctx = arg;

    ctx->ctrl_ops->progress_mmio(ctx->ctrl);
    return 0;
}

static inline int spdk_emu_progress_io(void *arg)
{
    struct spdk_emu_ctx *ctx = arg;

    return ctx->ctrl_ops->progress_io(ctx->ctrl);
}

static inline int spdk_emu_progress_io_thread(void *arg)
{
    struct spdk_emu_io_thread *thread = arg;
    struct spdk_emu_ctx *ctrl_ctx = thread->ctrl_ctx;

    return ctrl_ctx->ctrl_ops->progress_io_thread(ctrl_ctx->ctrl, thread->id);
}

static const struct spdk_emu_ctx_ctrl_ops spdk_emu_ctx_ctrl_ops_vrdma = {
    .prefix = VRDMA_EMU_NAME_PREFIX,

    .progress = vrdma_ctrl_adminq_progress,
    .progress_mmio = vrdma_ctrl_progress,
    .progress_io = vrdma_ctrl_progress_all_io,
    .progress_io_thread = vrdma_ctrl_progress_io,
    .suspend = vrdma_ctrl_suspend,
    .is_suspended = vrdma_ctrl_is_suspended,
    .resume = NULL
};

static int spdk_emu_ctrl_vrdma_create(struct spdk_emu_ctx *ctx,
                                const struct spdk_emu_ctx_create_attr *attr)
{
    struct vrdma_ctrl_init_attr vrdma_init_attr = {};
    struct spdk_emu_controller_vrdma_create_attr *vrdma_attr;

    SPDK_NOTICELOG("\n lizh spdk_emu_ctrl_vrdma_create..pf_id %d.start\n", attr->spci->id);
    vrdma_attr = attr->priv;
    vrdma_init_attr.emu_manager_name = attr->emu_manager;
    //vrdma_init_attr.pf_id = attr->spci->id;
    /* lizh hardcode for test */
    vrdma_init_attr.pf_id = attr->vdev->devid;
    vrdma_init_attr.nthreads = spdk_env_get_core_count();
    vrdma_init_attr.force_in_order = vrdma_attr->force_in_order;
    vrdma_init_attr.suspended = vrdma_attr->suspended;
    vrdma_init_attr.vdev = attr->vdev;
    ctx->ctrl = vrdma_ctrl_init(&vrdma_init_attr);
    if (!ctx->ctrl)
        goto err;

    ctx->ctrl_ops = &spdk_emu_ctx_ctrl_ops_vrdma;
    return 0;

err:
    SPDK_ERRLOG("failed to initialize VRDMA controller");
    return -1;
}

static bool spdk_emu_ctrl_has_mt(struct spdk_emu_ctx *ctx)
{
    return ctx->ctrl_ops->progress_io_thread &&
           spdk_env_get_core_count() > 1;
}

static void spdk_emu_ctx_destroy_end(void *arg)
{
    struct spdk_emu_ctx *ctx = arg;

    SPDK_NOTICELOG("Controller %s was destroyed\n", ctx->emu_name);

    /*
     * Before we finish, we must call fini_cb, since our caller may
     * expect to finish some operation once deletion is done.
     * Examples for such operations:
     *   1. send RPC completion.
     *   2. Call spdk_app_stop().
     */
    if (ctx->fini_cb)
        ctx->fini_cb(ctx->fini_cb_arg);
    free(ctx);
}

static void spdk_emu_ctx_destroy_mt_end(void *arg)
{
    struct spdk_emu_io_thread *thread = arg;
    struct spdk_emu_ctx *ctx = thread->ctrl_ctx;

    ctx->num_io_threads--;

    /* Only after all threads are done, it is safe to free resources */
    if (!ctx->num_io_threads) {
        free(ctx->io_threads);
        spdk_poller_unregister(&ctx->adminq_poller);
        spdk_poller_unregister(&ctx->bar_event_poller);
        /*
         * After IO pollers are all quiesced, it is now safe to
         * begin controller destruction
         */
        spdk_emu_ctrl_destroy(ctx, spdk_emu_ctx_destroy_end, ctx);
    }
}

static void spdk_emu_thread_unregister_poller(void *arg)
{
    struct spdk_emu_io_thread *thread = arg;

    spdk_poller_unregister(&thread->spdk_poller);
    spdk_thread_send_msg(thread->spdk_thread_creator,
                         spdk_emu_ctx_destroy_mt_end, thread);
}

static void spdk_emu_thread_register_poller(void *arg)
{
    struct spdk_emu_io_thread *thread = arg;

    thread->spdk_poller = spdk_poller_register(spdk_emu_progress_io_thread,
                                               thread, 0);
    if (!thread->spdk_poller)
        SPDK_ERRLOG("failed to register SPDK poller\n");
}

static void spdk_emu_ctx_destroy_mt_begin(struct spdk_emu_ctx *ctx)
{
    uint32_t i;
    struct spdk_emu_io_thread *thread;

    for (i = 0; i < ctx->num_io_threads; i++) {
        thread = &ctx->io_threads[i];

        spdk_thread_send_msg(thread->spdk_thread,
                             spdk_emu_thread_unregister_poller, thread);
    }
}

static void spdk_emu_ctrl_destroy_st_end(void *arg)
{
    struct spdk_emu_ctx *ctx = arg;

    spdk_thread_send_msg(spdk_get_thread(), spdk_emu_ctx_destroy_end, ctx);
}

static void spdk_emu_ctx_stop_pollers(struct spdk_emu_ctx *ctx)
{
    if (spdk_emu_ctrl_has_mt(ctx)) {
        spdk_emu_ctx_destroy_mt_begin(ctx);
    } else {
        spdk_poller_unregister(&ctx->io_poller);
        spdk_poller_unregister(&ctx->adminq_poller);
        spdk_poller_unregister(&ctx->bar_event_poller);
        spdk_emu_ctrl_destroy(ctx, spdk_emu_ctrl_destroy_st_end, ctx);
    }
}

void spdk_emu_ctx_destroy(struct spdk_emu_ctx *ctx)
{
    /* Before stopping all io threads give controller a chance to
     * finish all outstanding io requests. There is no need to use send_msg
     * because admin poller is running on the ctx_ctreate/destroy thread
    */
    ctx->should_stop = true;
    ctx->ctrl_ops->suspend(ctx->ctrl);

    if (ctx->ctrl_ops->is_suspended(ctx->ctrl)) {
        ctx->should_stop = false;
        spdk_emu_ctx_stop_pollers(ctx);
    }
}

struct spdk_emu_ctx *
spdk_emu_ctx_create(const struct spdk_emu_ctx_create_attr *attr)
{
    struct spdk_emu_ctx *ctx;
    int err;

    SPDK_NOTICELOG("\n lizh spdk_emu_ctx_create...start\n");
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        goto err;
    err = spdk_emu_ctrl_vrdma_create(ctx, attr);
    if (err)
        goto free_ctx;

    ctx->adminq_poller = spdk_poller_register(spdk_emu_progress, ctx, 100000);
    if (!ctx->adminq_poller) {
        SPDK_ERRLOG("failed to start controller admin queue poller\n");
        goto ctrl_destroy;
    }
    SPDK_NOTICELOG("\n lizh spdk_emu_ctx_create..adminq_poller...done\n");
    ctx->bar_event_poller = spdk_poller_register(spdk_emu_progress_mmio,
                                            ctx, 100000);
    if (!ctx->bar_event_poller) {
        SPDK_ERRLOG("failed to start controller bar event poller\n");
        goto unregister_adminq_poller;
    }
    SPDK_NOTICELOG("\n lizh spdk_emu_ctx_create..bar_event_poller...done\n");
    if (spdk_emu_ctrl_has_mt(ctx)) {
        err = spdk_emu_ctx_io_threads_create(ctx);
        if (err) {
            SPDK_ERRLOG("failed to start IO threads\n");
            goto unregister_adminq_bar_poller;
        }
    } else {
        ctx->io_poller = spdk_poller_register(spdk_emu_progress_io,
                                              ctx, 0);
        if (!ctx->io_poller) {
            SPDK_ERRLOG("failed to start general IO poller\n");
            goto unregister_adminq_bar_poller;
        }
    }

    ctx->spci = attr->spci;
    strncpy(ctx->emu_manager, attr->emu_manager,
            SPDK_EMU_MANAGER_NAME_MAXLEN - 1);
    if (attr->spci->parent)
        snprintf(ctx->emu_name, SPDK_EMU_NAME_MAXLEN,
                "%s%dpf%dvf%d", ctx->ctrl_ops->prefix,
                vrdma_dev_name_to_id(attr->emu_manager), attr->spci->parent->id,
                attr->spci->id);
    else
        snprintf(ctx->emu_name, SPDK_EMU_NAME_MAXLEN,
                "%s%dpf%d", ctx->ctrl_ops->prefix,
                vrdma_dev_name_to_id(attr->emu_manager), attr->spci->id);
    SPDK_NOTICELOG("\n lizh spdk_emu_ctx_create...done\n");
    return ctx;

unregister_adminq_bar_poller:
    spdk_poller_unregister(&ctx->adminq_poller);
unregister_adminq_poller:
    spdk_poller_unregister(&ctx->bar_event_poller);
ctrl_destroy:
    spdk_emu_ctrl_destroy(ctx, NULL, NULL);
free_ctx:
    free(ctx);
err:
    return NULL;
}

static int spdk_emu_ctx_io_threads_create(struct spdk_emu_ctx *ctrl_ctx)
{
    uint32_t i;
    struct spdk_emu_io_thread *thread;

    ctrl_ctx->num_io_threads = spdk_io_mgr_get_num_threads();
    ctrl_ctx->io_threads = calloc(ctrl_ctx->num_io_threads,
                                  sizeof(*ctrl_ctx->io_threads));
    if (!ctrl_ctx->io_threads)
        return -ENOMEM;

    for (i = 0; i < ctrl_ctx->num_io_threads; i++) {
        thread = &ctrl_ctx->io_threads[i];
        thread->spdk_thread = spdk_io_mgr_get_thread(i);
        thread->id = i;
        thread->ctrl_ctx = ctrl_ctx;
        thread->spdk_thread_creator = spdk_get_thread();
        spdk_thread_send_msg(thread->spdk_thread,
                             spdk_emu_thread_register_poller, thread);
    }

    return 0;
}

static void spdk_emu_ctrl_destroy(struct spdk_emu_ctx *ctx,
                                  void (*done_cb)(void *arg),
                                  void *done_cb_arg)
{
    ctx->ctrl_ops = NULL;
    SPDK_NOTICELOG("lizh spdk_emu_ctrl_destroy...start");
    vrdma_ctrl_destroy(ctx->ctrl, done_cb, done_cb_arg);
}

int
spdk_emu_controller_vrdma_create(struct spdk_vrdma_dev *vdev)
{
    struct spdk_emu_controller_vrdma_create_attr *attr = NULL;
    struct spdk_emu_ctx_create_attr emu_attr = {};
    struct spdk_emu_ctx *ctx;
    struct snap_pci *spci;

    SPDK_NOTICELOG("\n lizh spdk_emu_controller_vrdma_create start\n");
    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto err;
    strncpy(attr->emu_manager, ibv_get_device_name(vdev->emu_mgr),
             MAX_VRDMA_DEV_LEN - 1);
    attr->pf_id = vdev->devid;
    spci = spdk_vrdma_snap_get_snap_pci(attr->emu_manager, attr->pf_id);
    if (!spci)
        goto free_attr;
    SPDK_NOTICELOG("\n lizh spdk_emu_controller_vrdma_create emu_manager %s spci %p pf_id %d\n", attr->emu_manager, spci, attr->pf_id);
    pthread_mutex_lock(&spdk_emu_list_lock);
    ctx = spdk_emu_ctx_find_by_pci_id(attr->emu_manager,
                           attr->pf_id);
    if (ctx) {
        pthread_mutex_unlock(&spdk_emu_list_lock);
        SPDK_ERRLOG("PCI function is already in use by %s\n", ctx->emu_name);
        goto free_attr;
    }
    emu_attr.priv = attr;
    emu_attr.emu_manager = attr->emu_manager;
    emu_attr.spci = spci;
    emu_attr.vdev = vdev;
    ctx = spdk_emu_ctx_create(&emu_attr);
    if (!ctx) {
        pthread_mutex_unlock(&spdk_emu_list_lock);
        goto free_attr;
    }
    strncpy(vdev->emu_name, ctx->emu_name, MAX_VRDMA_DEV_LEN - 1);
    LIST_INSERT_HEAD(&spdk_emu_list, ctx, entry);
    pthread_mutex_unlock(&spdk_emu_list_lock);
    free(attr);
    SPDK_NOTICELOG("\n lizh spdk_emu_controller_vrdma_create done\n");
    return 0;

free_attr:
    free(attr);
err:
    SPDK_ERRLOG("failed to create VRDMA controller");
    return -1;
}

struct spdk_vrdma_rpc_controller_configue_attr {
    char *emu_manager;
    int dev_id;
    uint64_t mac;
    int dev_state;
    uint64_t adminq_paddr;
    uint32_t adminq_length;
    uint64_t dest_mac;
    int64_t subnet_prefix;
    int64_t intf_id;
    int vrdma_qpn;
    int backend_rqpn;
};

static const struct spdk_json_object_decoder
spdk_vrdma_rpc_controller_configue_decoder[] = {
    {
        "emu_manager",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, emu_manager),
        spdk_json_decode_string
    },
    {
        "dev_id",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dev_id),
        spdk_json_decode_int32
    },
    {
        "mac",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, mac),
        spdk_json_decode_uint64,
        true
    },
    {
        "dev_state",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dev_state),
        spdk_json_decode_int32,
        true
    },
    {
        "adminq_paddr",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, adminq_paddr),
        spdk_json_decode_uint64,
        true
    },
    {
        "adminq_length",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, adminq_length),
        spdk_json_decode_uint32,
        true
    },
    {
        "dest_mac",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, dest_mac),
        spdk_json_decode_uint64,
        true
    },
    {
        "subnet_prefix",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, subnet_prefix),
        spdk_json_decode_uint64,
        true
    },
    {
        "intf_id",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, intf_id),
        spdk_json_decode_uint64,
        true
    },
    {
        "vrdma_qpn",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, vrdma_qpn),
        spdk_json_decode_uint32,
        true
    },
    {
        "backend_rqpn",
        offsetof(struct spdk_vrdma_rpc_controller_configue_attr, backend_rqpn),
        spdk_json_decode_uint32,
        true
    },
};

/*lizh Just for test*/
static struct spdk_emu_ctx *
spdk_emu_ctx_find_by_pci_id_testrpc(const char *emu_manager, int pf_id)
{
    struct spdk_emu_ctx *ctx;

    LIST_FOREACH(ctx, &spdk_emu_list, entry) {
        
        SPDK_NOTICELOG("lizh spdk_emu_ctx_find_by_pci_id...%s type %d id %d",
        ctx->emu_manager, ctx->spci->type, ctx->spci->id);
        if (strncmp(ctx->emu_manager, emu_manager,
                    SPDK_EMU_MANAGER_NAME_MAXLEN))
            continue;
        if (ctx->spci->id == pf_id)
            return ctx;
    }
    return NULL;
}

static void
spdk_vrdma_rpc_controller_configue(struct spdk_jsonrpc_request *request,
                                    const struct spdk_json_val *params)
{
    struct spdk_vrdma_rpc_controller_configue_attr *attr = NULL;
    struct spdk_json_write_ctx *w;
    struct spdk_emu_ctx *ctx;
    struct snap_vrdma_ctrl *sctrl;
    struct vrdma_ctrl *ctrl;
    struct spdk_vrdma_qp *vqp;
    struct vrdma_backend_qp *bk_qp;
    uint32_t i;

    SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...start\n");
    attr = calloc(1, sizeof(*attr));
    if (!attr)
        goto invalid;

    // Set invalid index, to identify when value was not decoded
    attr->dev_id = -1;
    attr->dev_state = -1;
    attr->vrdma_qpn = -1;
    attr->backend_rqpn = -1;
    attr->subnet_prefix = -1;
    attr->intf_id = -1;

    if (spdk_json_decode_object(params,
            spdk_vrdma_rpc_controller_configue_decoder,
            SPDK_COUNTOF(spdk_vrdma_rpc_controller_configue_decoder),
            attr)) {
        SPDK_ERRLOG("Failed to decode parameters\n");
        goto free_attr;
    }
    if (attr->dev_id == -1 || !attr->emu_manager) {
        SPDK_ERRLOG("invalid device id -1\n");
        goto free_attr;
    }
    /* Find device data */
    ctx = spdk_emu_ctx_find_by_pci_id(attr->emu_manager,
                           attr->dev_id);
    if (!ctx) {
        /*lizh Just for test*/
        ctx = spdk_emu_ctx_find_by_pci_id_testrpc(attr->emu_manager,
                           attr->dev_id);
        if (!ctx) {
            SPDK_ERRLOG("Fail to find device for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
    }
    if (attr->mac) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...mac=0x%lx\n", attr->mac);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        sctrl = ctrl->sctrl;
        if (!sctrl) {
            SPDK_ERRLOG("Fail to find device snap controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (sctrl->bar_curr->status >= SNAP_VRDMA_DEVICE_S_DRIVER_OK) {
            SPDK_ERRLOG("Can not change MAC after driver_ok for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        sctrl->mac = attr->mac;
        g_bar_test.mac = attr->mac;
        if (snap_vrdma_device_mac_init(sctrl)) {
            SPDK_ERRLOG("Fail to change MAC after driver_ok for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
    }
    if (attr->dev_state != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...dev_state=0x%x\n", attr->dev_state);
        g_bar_test.status = attr->dev_state;
    }
    if (attr->adminq_paddr && attr->adminq_length) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...adminq_paddr=0x%lx adminq_length %d\n",
        attr->adminq_paddr, attr->adminq_length);
        g_bar_test.enabled = 1;
        g_bar_test.status = 4; /* driver_ok */
        g_bar_test.adminq_base_addr = attr->adminq_paddr;
        g_bar_test.adminq_size = attr->adminq_length;
    }
    if (attr->dest_mac) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...dest_mac=0x%lx\n", attr->dest_mac);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            SPDK_ERRLOG("Invalid vrdma_qpn for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
        if (!vqp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp = vqp->bk_qp[0];
        if (!bk_qp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        for (i = 0; i < 6; i++)
            bk_qp->dest_mac[5-i] = (attr->dest_mac >> (i * 8)) & 0xFF;
    }
    if (attr->backend_rqpn != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...backend_rqpn=0x%x\n", attr->backend_rqpn);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            SPDK_ERRLOG("Invalid vrdma_qpn for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
        if (!vqp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp = vqp->bk_qp[0];
        if (!bk_qp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp->remote_qpn = attr->backend_rqpn;
    }
    if (attr->subnet_prefix != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...subnet_prefix=0x%lx\n", attr->subnet_prefix);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            SPDK_ERRLOG("Invalid vrdma_qpn for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
        if (!vqp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp = vqp->bk_qp[0];
        if (!bk_qp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp->rgid_rip.global.subnet_prefix = attr->subnet_prefix;
    }
    if (attr->intf_id != -1) {
        SPDK_NOTICELOG("lizh spdk_vrdma_rpc_controller_configue...intf_id=0x%lx\n", attr->intf_id);
        ctrl = ctx->ctrl;
        if (!ctrl) {
            SPDK_ERRLOG("Fail to find device controller for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        if (attr->vrdma_qpn == -1) {
            SPDK_ERRLOG("Invalid vrdma_qpn for emu_manager %s\n", attr->emu_manager);
            goto free_attr;
        }
        vqp = find_spdk_vrdma_qp_by_idx(ctrl, attr->vrdma_qpn);
        if (!vqp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp = vqp->bk_qp[0];
        if (!bk_qp) {
            SPDK_ERRLOG("Fail to find vrdma_qpn %d's backend qp for emu_manager %s\n",
                    attr->vrdma_qpn, attr->emu_manager);
            goto free_attr;
        }
        bk_qp->rgid_rip.global.interface_id = attr->intf_id;
    }
    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, attr->emu_manager);
    spdk_jsonrpc_end_result(request, w);

    free(attr);
    return;

free_attr:
    free(attr);
invalid:
    spdk_jsonrpc_send_error_response(request,
                     SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                     "Invalid parameters");
}

SPDK_RPC_REGISTER("controller_vrdma_configue",
                  spdk_vrdma_rpc_controller_configue, SPDK_RPC_RUNTIME)
