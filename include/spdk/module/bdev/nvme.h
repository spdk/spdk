/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Nutanix Inc. All rights reserved.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
typedef void (*spdk_bdev_nvme_delete_cb)(void *ctx, int rc);

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
	const char *psk;
	const char *dhchap_key;
	const char *dhchap_ctrlr_key;

	/**
	 * Allow attaching namespaces with unrecognized command set identifiers.
	 * These will only support NVMe passthrough.
	 */
	bool allow_unrecognized_csi;

	/* Set to true if multipath enabled */
	bool multipath;
};

struct spdk_nvme_path_id;

enum spdk_bdev_timeout_action {
	SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE = 0,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT,
};

struct spdk_bdev_nvme_opts {
	/**
	 * The size of spdk_bdev_nvme_opts according to the caller of this library is used for ABI
	 * compatibility.  The library uses this field to know how many fields in this
	 * structure are valid. And the library will populate any remaining fields with default values.
	 * New added fields should be put at the end of the struct.
	 */
	size_t opts_size;
	enum spdk_bdev_timeout_action action_on_timeout;
	uint32_t keep_alive_timeout_ms;
	uint64_t timeout_us;
	uint64_t timeout_admin_us;
	/* The number of attempts per I/O in the transport layer before an I/O fails. */
	uint32_t transport_retry_count;
	uint32_t arbitration_burst;
	uint32_t low_priority_weight;
	uint32_t medium_priority_weight;
	uint32_t high_priority_weight;
	uint32_t io_queue_requests;
	uint64_t nvme_adminq_poll_period_us;
	uint64_t nvme_ioq_poll_period_us;
	bool delay_cmd_submit;
	/* Hole at bytes 73-75. */
	uint8_t reserved73[3];
	/* The number of attempts per I/O in the bdev layer before an I/O fails. */
	int32_t bdev_retry_count;
	int32_t ctrlr_loss_timeout_sec;
	uint32_t reconnect_delay_sec;
	uint32_t fast_io_fail_timeout_sec;
	uint8_t transport_ack_timeout;
	bool disable_auto_failback;
	bool generate_uuids;
	/* Type of Service - RDMA only */
	uint8_t transport_tos;
	bool nvme_error_stat;
	bool io_path_stat;
	bool allow_accel_sequence;
	/* Hole at byte 99. */
	uint8_t reserved99[1];
	uint32_t rdma_srq_size;
	uint32_t rdma_max_cq_size;
	uint16_t rdma_cm_event_timeout_ms;
	/* Hole at bytes 110-111. */
	uint8_t reserved110[2];
	uint32_t dhchap_digests;
	uint32_t dhchap_dhgroups;
	bool rdma_umr_per_io;
	/* Hole at bytes 121-123. */
	uint8_t reserved121[3];
	uint32_t tcp_connect_timeout_ms;
	bool enable_flush;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_nvme_opts) == 136, "Incorrect size");

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
 * \return 0 on success, negative errno on failure.
 */
int spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  const char **names,
			  uint32_t count,
			  spdk_bdev_nvme_create_cb cb_fn,
			  void *cb_ctx,
			  struct spdk_nvme_ctrlr_opts *drv_opts,
			  struct spdk_bdev_nvme_ctrlr_opts *bdev_opts);

/**
 * Delete the specified NVMe controller, or one of its paths.
 *
 * NOTE: When path_id is specified and it is the only path_id associated with NVMe controller
 * the path is removed and the NVMe controller gets deleted. (Optional) callback
 * function gets executed on delete complete in caller's thread. When the (optional)
 * callback is not provided, the control is returned back at the time delete is initiated,
 * not when it is completed. When NVMe controller deletion is already in progress state,
 * this function returns success.
 *
 * \param name NVMe controller name.
 * \param path_id The specified path to remove (optional).
 * \param delete_cb	Callback function on delete complete (optional).
 * \param cb_ctx Context passed to callback (optional).
 * \return zero on success,
 *		-EINVAL on wrong parameters or
 *		-ENODEV if controller is not found or
 *		-ENOMEM on no memory
 */
int spdk_bdev_nvme_delete(const char *name, const struct spdk_nvme_path_id *path_id,
			  spdk_bdev_nvme_delete_cb delete_cb, void *cb_ctx);

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

/**
 * Get the default value for bdev nvme options.
 *
 * \param[out] opts Bdev nvme options object to be filled with default values.
 * \param opts_size Must be set to sizeof(struct spdk_bdev_nvme_opts).
 */
void spdk_bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts, size_t opts_size);

/**
 * Set the bdev nvme options.
 *
 * \param opts New value of bdev nvme options to be set.
 * \return 0 on success, negative errno on failure.
 */
int spdk_bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_NVME_H_ */
