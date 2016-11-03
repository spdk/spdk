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

#ifndef SPDK_BLOCKDEV_NVME_H
#define SPDK_BLOCKDEV_NVME_H

#include <stdint.h>

#include "spdk/bdev.h"
#include "spdk/env.h"

#define NVME_MAX_CONTROLLERS 16
#define NVME_MAX_BLOCKDEVS_PER_CONTROLLER 256
#define NVME_MAX_BLOCKDEVS (NVME_MAX_BLOCKDEVS_PER_CONTROLLER * NVME_MAX_CONTROLLERS)

struct nvme_probe_ctx {
	int controllers_remaining;
	int num_whitelist_controllers;
	struct spdk_pci_addr whitelist[NVME_MAX_CONTROLLERS];

	/* Filled by spdk_bdev_nvme_create() with the bdevs that were added */
	int num_created_bdevs;
	struct spdk_bdev *created_bdevs[NVME_MAX_BLOCKDEVS];
};

int
spdk_bdev_nvme_create(struct nvme_probe_ctx *ctx);

#endif // SPDK_BLOCKDEV_NVME_H
