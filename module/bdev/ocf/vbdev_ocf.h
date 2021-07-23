/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_OCF_H
#define SPDK_VBDEV_OCF_H

#include <ocf/ocf.h>

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

#define VBDEV_OCF_MD_MAX_LEN 4096

struct vbdev_ocf;

/* Context for OCF queue poller
 * Used for mapping SPDK threads to OCF queues */
struct vbdev_ocf_qctx {
	/* OCF queue. Contains OCF requests */
	struct ocf_queue            *queue;
	/* Poller for OCF queue. Runs OCF requests */
	struct spdk_poller          *poller;
	/* Reference to parent vbdev */
	struct vbdev_ocf            *vbdev;
	/* Base devices channels */
	struct spdk_io_channel      *cache_ch;
	struct spdk_io_channel      *core_ch;
	/* If true, we have to free this context on queue stop */
	bool allocated;
	/* Link to per-bdev list of queue contexts */
	TAILQ_ENTRY(vbdev_ocf_qctx)  tailq;
};

/* Important states */
struct vbdev_ocf_state {
	/* From the moment when clean delete started */
	bool                         doing_clean_delete;
	/* From the moment when finish started */
	bool                         doing_finish;
	/* From the moment when reset IO received, until it is completed */
	bool                         doing_reset;
	/* From the moment when exp_bdev is registered */
	bool                         started;
	/* From the moment when register path started */
	bool                         starting;
	/* Status of last attempt for stopping this device */
	int                          stop_status;
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

	/* Load flag, if set to true, then we will try load cache instance from disk,
	 * otherwise we will create new cache on that disk */
	bool                                loadq;
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

	/* Function that gets invoked by poller on each iteration */
	vbdev_ocf_mngt_fn                   poller_fn;
	/* Poller timeout time stamp - when the poller should stop with error */
	uint64_t                            timeout_ts;

	/* Status of management operation */
	int                                 status;

	/* External callback and its argument */
	vbdev_ocf_mngt_callback             cb;
	void                               *cb_arg;
};

/* Base device info */
struct vbdev_ocf_base {
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

	/* Channel for cleaner operations */
	struct spdk_io_channel      *management_channel;

	/* Reference to main vbdev */
	struct vbdev_ocf            *parent;

	/* thread where base device is opened */
	struct spdk_thread	    *thread;
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

	/* Cache context */
	struct vbdev_ocf_cache_ctx  *cache_ctx;

	/* Status of flushing operation */
	struct {
		bool in_progress;
		int status;
	} flush;

	/* Exposed SPDK bdev. Registered in bdev layer */
	struct spdk_bdev             exp_bdev;

	/* OCF uuid for core device of this vbdev */
	char uuid[VBDEV_OCF_MD_MAX_LEN];

	/* Link to global list of this type structures */
	TAILQ_ENTRY(vbdev_ocf)       tailq;
};

void vbdev_ocf_construct(
	const char *vbdev_name,
	const char *cache_mode_name,
	const uint64_t cache_line_size,
	const char *cache_name,
	const char *core_name,
	bool loadq,
	void (*cb)(int, struct vbdev_ocf *, void *),
	void *cb_arg);

/* If vbdev is online, return its object */
struct vbdev_ocf *vbdev_ocf_get_by_name(const char *name);

/* Return matching base if parent vbdev is online */
struct vbdev_ocf_base *vbdev_ocf_get_base_by_name(const char *name);

/* Stop OCF cache and unregister SPDK bdev */
int vbdev_ocf_delete(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg);

int vbdev_ocf_delete_clean(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg);

/* Set new cache mode on OCF cache */
void vbdev_ocf_set_cache_mode(
	struct vbdev_ocf *vbdev,
	const char *cache_mode_name,
	void (*cb)(int, struct vbdev_ocf *, void *),
	void *cb_arg);

/* Set sequential cutoff parameters on OCF cache */
void vbdev_ocf_set_seqcutoff(
	struct vbdev_ocf *vbdev,
	const char *policy_name,
	uint32_t threshold,
	uint32_t promotion_count,
	void (*cb)(int, void *),
	void *cb_arg);

typedef void (*vbdev_ocf_foreach_fn)(struct vbdev_ocf *, void *);

/* Execute fn for each OCF device that is online or waits for base devices */
void vbdev_ocf_foreach(vbdev_ocf_foreach_fn fn, void *ctx);

#endif
