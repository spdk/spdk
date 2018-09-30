/*
 * INTEL CONFIDENTIAL
 *
 * Copyright 2018 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you (License). Unless the License provides otherwise, you may not
 * use, modify, copy, publish, distribute, disclose or transmit this software or
 * the related documents without Intel's prior written permission.
 * This software and the related documents are provided as is, with no express or
 * implied warranties, other than those that are expressly stated in the License.
 */

#ifndef SPDK_BDEV_TARGET_H
#define SPDK_BDEV_TARGET_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev_target;
struct spdk_bdev_aio_ctx;
struct spdk_bdev_aio_req;
struct spdk_nvme_cmd;

/**
 * Setup essential SPDK environment for bdev target.
 *
 * @note
 * The function should be called by upper application outside SPDK,
 * before opening bdev target.
 * It will spawn out spdk thread pinning on CPU core, and provide
 * target service.
 * Repeated calling needs repeated unset which means there is a internal
 * reference counter to record setup times. *
 *
 * @param config_file SPDK configuration file path
 * @param debug Enable debug logs
 * @returns 0 on success. On error: returns -errno
 */
int spdk_env_setup(char *config_file, bool debug);

/**
 * Unset SPDK environment.
 *
 * @note
 * The function should be called by upper application outside SPDK, after
 * the usage of bdev_target.
 * It will send shutdown signal to SPDK thread to stop the service.
 */
void spdk_env_unset(void);

/**
 * Open one bdev target.
 * Note: Repeated opening needs repeated close.
 *
 * @param bdev_name Name of NVMe NS bdev device, eg: Nvme0n1.
 * @param bt Opened bdev_target will be retured to bt.
 * @returns 0 on success. On error: returns -1
 */
int spdk_bt_open(char *bdev_name, struct spdk_bdev_target **bt);

/**
 * Close one bdev target.
 *
 * Note: Before close bt, ctx set up from this bt should be destroyed.
 *
 * @param bt Opened bdev_target.
 */
void spdk_bt_close(struct spdk_bdev_target *bt);


/**
 * Encapsulation and representation of lower-level error conditions
 */
struct spdk_bdev_ret {
	uint64_t status;	///< NVMe command status / completion bits
	uint32_t result;	///< NVMe command error codes
};

/**
 * Get ret from aio request.
 *
 * @param req Ptr of req.
 * @returns The result and status of the aio request
 */
struct spdk_bdev_ret *spdk_bdev_aio_req_get_ret(struct spdk_bdev_aio_req *req);

/**
 * Get size of aio request.
 *
 * @returns The size of aio request
 */
int spdk_bdev_aio_req_size(void);

static inline int spdk_bdev_aio_ret_check(struct spdk_bdev_ret *ret)
{
	if (ret->status != 0 || ret->result != 0) {
	        return -EIO;
	}

	return 0;
}

/**
 * Create an asynchronous I/O context for bdev target
 *
 * @note
 * Any io/admin request should be submitted with a ctx.
 *
 * @param ctx Ptr of ctx which should be NULL and it is allocated inside.
 * @param bt bdev_target
 * @returns 0 on success. On error: returns -1
 */
int spdk_bdev_aio_ctx_setup(struct spdk_bdev_aio_ctx **ctx, struct spdk_bdev_target *bt);

/**
 * Read asynchronous I/O events from the completion queue
 *
 * @note
 * It attempts to read at least min_nr reqs and up to nr reqs from the completion queue
 * of the AIO context specified by ctx. The timeout argument specifies the amount of time
 * to wait for reqs, where a NULL timeout waits until at least min_nr reqs have been seen.
 * Note that timeout is not implemented.
 *
 * @param ctx Ptr of ctx which is allocated outside.
 * @param nr_min minimum number of reqs. if 0, then all reqs should be completed.
 * @param nr maxinum number of reqs
 * @param reqs array to contain completed reqs.
 * @param timeout Waiting time (Not implemented): NULL waits until at least min_nr events have been completed.
 * if timeout is {0, 0}, then it will return directly.
 * @returns On error: returns -errno; On success: returns number of gotten reqs.
 */
int spdk_bdev_aio_ctx_get_reqs(struct spdk_bdev_aio_ctx *ctx,
		int nr_min, int nr, struct spdk_bdev_aio_req *reqs[],
		struct timespec *timeout);

/**
 * Destroy the context.
 *
 * @note
 * Wait all outstanding asynchronous I/O operations against ctx to be completed, and will destroy the ctx.
 *
 * @param ctx Ptr of ctx which is allocated outside.
 * @returns On error: returns -errno; On success: returns number of gotten reqs.
 */
int spdk_bdev_aio_ctx_destroy(struct spdk_bdev_aio_ctx *ctx);

/**
 * Submit reqs to context.
 *
 * @note
 * Queues nr I/O requests for processing in the AIO context ctx.
 * If ctx is in the process of destroy, submit will return with error directly.
 *
 * @param ctx Ptr of ctx which is allocated outside.
 * @param nr Number of reqs
 * @param reqs Array to req pointers.
 * @returns	On success: returns number of queued reqs.
 * 		On error: returns -errno;
 */
int spdk_bdev_aio_ctx_submit(struct spdk_bdev_aio_ctx *ctx,
		int nr, struct spdk_bdev_aio_req *reqs[]);

/**
 * Set one req with NVMe admin passthru command type.
 *
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer.
 * @param pin_buf Data buffer related to admin command, it should be DMAable
 * @param data_len Length of data buffer
 */
void spdk_bdev_aio_req_set_admin_passthru(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_cmd *cmd, void *pin_buf, size_t data_len);

/**
 * Set one req with NVMe IO passthru command type.
 *
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer. Its content will be copied into req.
 * @param pin_buf Data buffer related to IO command, it should be DMAable
 * @param data_len Length of data buffer
 * @param pin_meta Metadata buffer related to IO command, it should be DMAable
 * @param md_len Length of metadata buffer
 */
void spdk_bdev_aio_req_set_io_passthru(struct spdk_bdev_aio_req *req, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len);

/**
 * Get one private argument from aio request.
 *
 * @param req Ptr of req.
 * @returns private_arg Private argument.
 */
void *spdk_bdev_aio_req_get_private_arg(struct spdk_bdev_aio_req *req);

/**
 * Set one private argument to aio request.
 *
 * @param req Ptr of req.
 * @param private_arg Private argument.
 */
void spdk_bdev_aio_req_set_private_arg(struct spdk_bdev_aio_req *req, void *private_arg);

typedef void (*spdk_bdev_aio_req_complete_cb)(void *cb_arg, int bterrno, struct spdk_bdev_ret *nvm_ret);

/**
 * Set up one specific callback function and its argument to req.
 * Note: the callback will be called on SPDK polling core when the request is
 * completed. Callback
 *
 * @param req Ptr of req.
 * @param cb Specific callback function.
 * @param cb_arg Argument for callback function.
 */
void spdk_bdev_aio_req_set_cb(struct spdk_bdev_aio_req *req, spdk_bdev_aio_req_complete_cb cb, void *cb_arg);

/**
 * Submit reqs with callbacks.
 *
 * @note
 * Queues nr I/O requests for processing.
 *
 * @param bt bdev_target
 * @param nr Number of reqs
 * @param reqs Array to req pointers.
 * @returns	On success: returns number of submitted reqs.
 * 		On error: returns -errno;
 */
int spdk_bdev_aio_cb_submit(struct spdk_bdev_target *bt, int nr, struct spdk_bdev_aio_req *reqs[]);

/**
 * Execute one req with NVMe admin passthru command type.
 *
 * @param bt bdev_target
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer.
 * @param pin_buf Data buffer related to admin command, it should be DMAable
 * @param data_len Length of data buffer
 * @returns On error: returns -errno; On success: returns 0
 */
int spdk_bdev_req_admin_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, struct spdk_bdev_ret *ret);

/**
 * Prepare one req with NVMe IO passthru command type.
 *
 * @param bt bdev_target
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer.
 * @param pin_buf Data buffer related to IO command, it should be DMAable
 * @param data_len Length of data buffer
 * @param pin_meta Metadata buffer related to IO command, it should be DMAable
 * @param md_len Length of metadata buffer
 * @returns On error: returns -errno; On success: returns 0
 */
int spdk_bdev_req_io_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len, struct spdk_bdev_ret *ret);

#ifdef __cplusplus
}
#endif

#endif // SPDK_BDEV_TARGET_H
