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

#ifndef SPDK_VBDEV_OCF_H
#define SPDK_VBDEV_OCF_H

#include <ocf/ocf.h>

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

struct vbdev_ocf;

/* Context for OCF queue poller
 * Used for mapping SPDK threads to OCF queues */
struct vbdev_ocf_qcxt {
	/* OCF queue. Contains OCF requests */
	struct ocf_queue            *queue;
	/* Poller for OCF queue. Runs OCF requests */
	struct spdk_poller          *poller;
	/* Reference to parent vbdev */
	struct vbdev_ocf            *vbdev;
	/* Base devices channels */
	struct spdk_io_channel      *cache_ch;
	struct spdk_io_channel      *core_ch;
	/* Link to per-bdev list of queue contexts */
	TAILQ_ENTRY(vbdev_ocf_qcxt)  tailq;
};

/* Important states */
struct vbdev_ocf_state {
	/* From the moment when finish started */
	bool                         doing_finish;
	/* From the moment when reset IO recieved, until it is completed */
	bool                         doing_reset;
	/* From the moment when exp_bdev is registered */
	bool                         started;
};

/*
 * OCF cache configuration options
 */
struct vbdev_ocf_config {
	/* Initial cache configuration  */
	struct ocf_mngt_cache_config        cache;

	/* Cache device config */
	struct ocf_mngt_cache_device_config device;

	/* Core initial config */
	struct ocf_mngt_core_config         core;
};

/* Types for management operations */
typedef void (*vbdev_ocf_mngt_fn)(struct vbdev_ocf *);
typedef void (*vbdev_ocf_mngt_callback)(int, struct vbdev_ocf *, void *);

/* Context for asynchronous management operations
 * Single management operation usually contains a list of sub procedures,
 * this structure handles sharing between those sub procedures */
struct vbdev_ocf_mngt_ctx {
	/* Pointer to function that is currently being executed
	 * It gets incremented on each step until it dereferences to NULL */
	vbdev_ocf_mngt_fn                  *current_step;

	/* Poller, registered once per whole management operation */
	struct spdk_poller                 *poller;
	/* Function that gets invoked by poller on each iteration */
	vbdev_ocf_mngt_fn                   poller_fn;

	/* Status of management operation */
	int                                 status;

	/* External callback and its argument */
	vbdev_ocf_mngt_callback             cb;
	void                               *cb_arg;
};

/* Base device info */
struct vbdev_ocf_base {
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

	/* True if SPDK bdev has been claimed and opened for writing */
	bool                         attached;

	/* Reference to main vbdev */
	struct vbdev_ocf            *parent;
};

/*
 * The main information provider
 * It's also registered as io_device
 */
struct vbdev_ocf {
	/* Exposed unique name */
	char                        *name;

	/* Base bdevs */
	struct vbdev_ocf_base        cache;
	struct vbdev_ocf_base        core;

	/* Base bdevs OCF objects */
	ocf_cache_t                  ocf_cache;
	ocf_core_t                   ocf_core;

	/* Parameters */
	struct vbdev_ocf_config      cfg;
	struct vbdev_ocf_state       state;

	/* Management context */
	struct vbdev_ocf_mngt_ctx    mngt_ctx;

	/* Exposed SPDK bdev. Registered in bdev layer */
	struct spdk_bdev             exp_bdev;

	/* Link to global list of this type structures */
	TAILQ_ENTRY(vbdev_ocf)       tailq;
};

void vbdev_ocf_construct(
	const char *vbdev_name,
	const char *cache_mode_name,
	const char *cache_name,
	const char *core_name,
	void (*cb)(int, struct vbdev_ocf *, void *),
	void *cb_arg);

/* If vbdev is online, return its object */
struct vbdev_ocf *vbdev_ocf_get_by_name(const char *name);

/* Return matching base if parent vbdev is online */
struct vbdev_ocf_base *vbdev_ocf_get_base_by_name(const char *name);

/* Stop OCF cache and unregister SPDK bdev */
int vbdev_ocf_delete(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg);

typedef void (*vbdev_ocf_foreach_fn)(struct vbdev_ocf *, void *);

/* Execute fn for each OCF device that is online or waits for base devices */
void vbdev_ocf_foreach(vbdev_ocf_foreach_fn fn, void *ctx);

#endif
