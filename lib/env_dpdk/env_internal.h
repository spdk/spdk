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

#ifndef SPDK_ENV_INTERNAL_H
#define SPDK_ENV_INTERNAL_H

#define spdk_pci_device rte_pci_device

#define _spdk_memconfig rte_memconfig
#define _spdk_lcore_id  rte_lcore_id
#define _spdk_mempool_put  rte_mempool_put
#define _spdk_mempool_create rte_mempool_create
#define _spdk_get_master_lcore rte_get_master_lcore
#define _SPDK_MAX_LCORE RTE_MAX_LCORE

#include "spdk/env.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdbool.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_version.h>
#include <rte_dev.h>

struct spdk_pci_enum_ctx {
	struct rte_pci_driver	driver;
	spdk_pci_enum_cb	cb_fn;
	void 			*cb_arg;
	pthread_mutex_t		mtx;
	bool			is_registered;
};

int spdk_pci_device_init(struct rte_pci_driver *driver, struct rte_pci_device *device);
int spdk_pci_device_fini(struct rte_pci_device *device);

int spdk_pci_enumerate(struct spdk_pci_enum_ctx *ctx, spdk_pci_enum_cb enum_cb, void *enum_ctx);
int spdk_pci_device_attach(struct spdk_pci_enum_ctx *ctx, spdk_pci_enum_cb enum_cb, void *enum_ctx,
			   struct spdk_pci_addr *pci_address);

void spdk_vtophys_register_dpdk_mem(void);

#endif
