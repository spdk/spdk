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

#ifndef _VRDMA_EMU_MGR_H
#define _VRDMA_EMU_MGR_H
#include <stdio.h>
#include <stdint.h>
#include "spdk/vrdma.h"

#define SPDK_EMU_NAME_MAXLEN MAX_VRDMA_DEV_LEN
#define SPDK_EMU_MANAGER_NAME_MAXLEN 16

struct snap_pci;

struct spdk_emu_io_thread {
    int id;
    struct spdk_emu_ctx *ctrl_ctx;
    struct spdk_thread *spdk_thread;
    struct spdk_thread *spdk_thread_creator;
    struct spdk_poller *spdk_poller;
};

struct spdk_emu_ctx {
    void *ctrl;
    const struct spdk_emu_ctx_ctrl_ops *ctrl_ops;
    char emu_manager[SPDK_EMU_MANAGER_NAME_MAXLEN];
    struct snap_pci *spci;
    char emu_name[SPDK_EMU_NAME_MAXLEN];
    struct spdk_poller *adminq_poller;
    struct spdk_poller *bar_event_poller;
    struct spdk_poller *io_poller;
    size_t num_io_threads;
    struct spdk_emu_io_thread *io_threads;
    LIST_ENTRY(spdk_emu_ctx) entry;

    /* Callback to be called after ctx is destroyed */
    void (*fini_cb)(void *arg);
    void *fini_cb_arg;

    bool should_stop;
};

struct spdk_emu_ctx_create_attr {
    void *priv;
    const char *emu_manager;
    struct snap_pci *spci;
    struct spdk_vrdma_dev *vdev;
};

LIST_HEAD(spdk_emu_list_head, spdk_emu_ctx);

extern struct spdk_emu_list_head spdk_emu_list;
extern pthread_mutex_t spdk_emu_list_lock;

struct spdk_emu_ctx *
spdk_emu_ctx_find_by_pci_id(const char *emu_manager, int pf_id);
struct spdk_emu_ctx *
spdk_emu_ctx_find_by_gid_ip(const char *emu_manager, uint64_t gid_ip);
struct spdk_emu_ctx *spdk_emu_ctx_find_by_emu_name(const char *emu_name);

struct spdk_emu_ctx *
spdk_emu_ctx_create(const struct spdk_emu_ctx_create_attr *attr);
int spdk_emu_controller_vrdma_create(struct spdk_vrdma_dev *vdev);
void spdk_emu_ctx_destroy(struct spdk_emu_ctx *ctx);

#endif
