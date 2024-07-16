/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Nutanix Inc. All rights reserved.
 */

/** \file
 * Nvme block device abstraction layer
 */

#ifndef SPDK_MODULE_BDEV_NVME_H_
#define SPDK_MODULE_BDEV_NVME_H_

#include "spdk/nvme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spdk_bdev_nvme_create_cb)(void *ctx, size_t bdev_count, int rc);
typedef void (*spdk_bdev_nvme_set_multipath_policy_cb)(void *cb_arg, int rc);

enum spdk_bdev_nvme_multipath_policy {
	BDEV_NVME_MP_POLICY_ACTIVE_PASSIVE,
	BDEV_NVME_MP_POLICY_ACTIVE_ACTIVE,
};

enum spdk_bdev_nvme_multipath_selector {
	BDEV_NVME_MP_SELECTOR_ROUND_ROBIN = 1,
	BDEV_NVME_MP_SELECTOR_QUEUE_DEPTH,
};

struct spdk_bdev_nvme_ctrlr_opts {
	uint32_t prchk_flags;
	int32_t ctrlr_loss_timeout_sec;
	uint32_t reconnect_delay_sec;
	uint32_t fast_io_fail_timeout_sec;
	bool from_discovery_service;
	/* Name of the PSK or path to the file containing PSK. */
	char psk[PATH_MAX];
	const char *dhchap_key;
	const char *dhchap_ctrlr_key;

	/**
	 * Allow attaching namespaces with unrecognized command set identifiers.
	 * These will only support NVMe passthrough.
	 */
	bool allow_unrecognized_csi;
};

/**
 * Connect to the NVMe controller and populate namespaces as bdevs.
 *
 * \param trid Transport ID for nvme controller.
 * \param base_name Base name for the nvme subsystem.
 * \param names Pointer to string array to get bdev names.
 * \param count Maximum count of the string array 'names'. Restricts the length
 *		of 'names' array only, not the count of bdevs created.
 * \param cb_fn Callback function to be called after all the bdevs are created
 *              or updated if already created.
 * \param cb_ctx Context to pass to cb_fn.
 * \param drv_opts NVMe driver options.
 * \param bdev_opts NVMe bdev options.
 * \param multipath Whether to enable multipathing (if true) else failover mode.
 * \return 0 on success, negative errno on failure.
 */
int spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  const char **names,
			  uint32_t count,
			  spdk_bdev_nvme_create_cb cb_fn,
			  void *cb_ctx,
			  struct spdk_nvme_ctrlr_opts *drv_opts,
			  struct spdk_bdev_nvme_ctrlr_opts *bdev_opts,
			  bool multipath);

/**
 * Set multipath policy of the NVMe bdev.
 *
 * \param name NVMe bdev name.
 * \param policy Multipath policy (active-passive or active-active).
 * \param selector Multipath selector (round_robin, queue_depth).
 * \param rr_min_io Number of IO to route to a path before switching to another for round-robin.
 * \param cb_fn Function to be called back after completion.
 * \param cb_arg Argument passed to the callback function.
 */
void spdk_bdev_nvme_set_multipath_policy(const char *name,
		enum spdk_bdev_nvme_multipath_policy policy,
		enum spdk_bdev_nvme_multipath_selector selector,
		uint32_t rr_min_io,
		spdk_bdev_nvme_set_multipath_policy_cb cb_fn,
		void *cb_arg);

/* Get default values for controller opts.
 *
 * \param opts Ctrlr opts object to be loaded with default values.
 */
void spdk_bdev_nvme_get_default_ctrlr_opts(struct spdk_bdev_nvme_ctrlr_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_NVME_H_ */
