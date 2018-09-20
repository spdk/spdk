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

#ifndef SPDK_VBDEV_CACHE_H
#define SPDK_VBDEV_CACHE_H

#include <ocf/ocf.h>

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

struct vbdev_cas;

/* Important non-deterministic state infos */
struct vbdev_cas_state {
	bool                         doing_finish;
	bool                         doing_reset;
	int                          unfinished_io_cnt;
};

/*
 * OCF cache configuration options
 */
struct vbdev_cas_config {
	/* Initial cache configuration  */
	struct ocf_mngt_cache_config        cache;

	/* Cache device config */
	struct ocf_mngt_cache_device_config device;

	/* Core initial config */
	struct ocf_mngt_core_config         core;
};

/* Base device info */
struct vbdev_cas_base {
	/* OCF unique internal id */
	int                          id;

	/* OCF internal name */
	char                        *name;

	/* True if this is a caching device */
	bool                         is_cache;

	/* Connected SPDK block device */
	struct spdk_bdev            *bdev;

	/* SPDK device io handle */
	struct spdk_bdev_desc       *desc;

	/* True if SPDK bdev has been assigned to this base */
	bool                         attached;

	/* Reference to main vbdev */
	struct vbdev_cas            *parent;

	/* TODO [multichannel0]: initialize this per-queue */
	/* Keeping this because OCF stack does not allow passing ctx with io */
	struct spdk_io_channel      *base_channel;
};

/*
 * The main information provider
 * It's also registered as io_device
 */
struct vbdev_cas {
	/* Exposed unique name */
	char                        *name;

	/* Base bdevs */
	struct vbdev_cas_base        cache;
	struct vbdev_cas_base        core;

	/* Base bdevs OCF objects */
	ocf_cache_t                  ocf_cache;
	ocf_core_t                   ocf_core;

	/* Parameters */
	struct vbdev_cas_config      cfg;
	struct vbdev_cas_state       state;

	/* Exposed SPDK bdev. Registered in bdev layer */
	struct spdk_bdev             exp_bdev;

	/* Link to global list of this type structures */
	TAILQ_ENTRY(vbdev_cas)       tailq;
};

int vbdev_cas_construct(
	const char *vbdev_name,
	const char *cache_mode_name,
	const char *cache_name,
	const char *core_name);

struct vbdev_cas *vbdev_cas_get_by_name(const char *name);

#endif
