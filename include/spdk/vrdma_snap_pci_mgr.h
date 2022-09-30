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

#ifndef _VRDMA_SNAP_PCI_MGR_H
#define _VRDMA_SNAP_PCI_MGR_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
//#include <infiniband/verbs.h>
//#include "snap.h"

struct snap_pci;
struct snap_context;
struct ibv_context;
struct ibv_pd;
struct ibv_device;

struct snap_pci **spdk_vrdma_snap_get_snap_pci_list(const char *vrdma_dev,
                                              int *spci_list_sz);
struct snap_context *spdk_vrdma_snap_get_snap_context(const char *vrdma_dev);
struct ibv_context *spdk_vrdma_snap_get_ibv_context(const char *vrdma_dev);
struct ibv_pd *spdk_vrdma_snap_get_ibv_pd(const char *vrdma_dev);
struct ibv_device *spdk_vrdma_snap_get_ibv_device(const char *vrdma_dev);
struct snap_pci *spdk_vrdma_snap_get_snap_pci(const char *vrdma_dev, int pf_index);

int spdk_vrdma_snap_pci_mgr_init(void);
void spdk_vrdma_snap_pci_mgr_clear(void);
#endif
