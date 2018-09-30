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

/**
 * Setup essential SPDK environment for bdev target.
 *
 * @note
 * The function should be called by upper application outside SPDK,
 * before opening bdev target.
 * It will spawn out spdk thread pinning on CPU core, and provide
 * target service.
 *
 * @param config_file SPDK configuration file path
 * @returns 0 on success. On error: returns -errno
 */
int spdk_env_setup(char *config_file);

/**
 * Unset SPDK environment.
 *
 * @note
 * The function should be called by upper application outside SPDK, after
 * the usage of bdev_target.
 * It will send shutdown signal to SPDK thread to stop the service.
 */
void spdk_env_unset(void);

struct spdk_bdev_target;

/**
 * Open one bdev target.
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

struct spdk_bdev_aio_ctx;
struct spdk_bdev_aio_req;

typedef void (*spdk_bdev_aio_req_complete_cb)(void *cb_arg, int bterrno, struct spdk_bdev_ret *nvm_ret);
typedef void (*spdk_bdev_aio_get_reqs_cb)(void *cb_arg);
typedef void (*spdk_bdev_aio_queue_req_cb)(void *cb_arg);

/* batch io start */
struct spdk_bdev_aio_req {
	struct spdk_bdev_aio_ctx *ctx;
	struct spdk_bdev_ret ret;
	union {
		struct {
			void		*pin_buf;
			void		*pin_meta;
			uint64_t	ppa;
			uint32_t	num_lbas;
			uint16_t	io_flags;

			bool		is_read;
		} rw;
		struct {
			struct spdk_nvme_cmd cmd;
			void		*pin_buf;
			void		*pin_meta;
			uint32_t	data_len;
			uint32_t	md_len;

			bool		is_admin;
		} passthru;
	} op;

	int req_rc;

	/* func pointer if req has it's own notify routine */
	spdk_bdev_aio_req_complete_cb user_complete_cb;
	void *complete_cb_arg;

	/* func pointer used to queue req into bdev */
	spdk_bdev_aio_queue_req_cb queue_req_fn;
	TAILQ_ENTRY(spdk_bdev_aio_req) req_tailq;

	void* private_data;
};

struct spdk_bdev_aio_get_reqs_ctx{
	struct spdk_bdev_aio_ctx *ctx;

	bool all;
	int nr_min;
	int nr;
	struct spdk_bdev_aio_req **reqs;

	spdk_bdev_aio_get_reqs_cb get_reqs_cb;
	void *get_reqs_cb_arg;
	int get_reqs_rc;
};

struct spdk_bdev_aio_ctx {
	uint32_t bdev_core;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*bdev_spdk_io_channel;
	struct spdk_bdev_target *bt;

	pthread_spinlock_t ctx_lock;
	int reqs_submitting;	// number of requests haven't been submitted into bdev
	int reqs_submitted;	// number of requests have been submitted into bdev, but haven't been completed
	int reqs_completed;	// number of requests have been completed, but haven't been realized
	TAILQ_HEAD(req_submitting_list, spdk_bdev_aio_req) submitting_list;
	TAILQ_HEAD(, spdk_bdev_aio_req) completed_list;

	struct spdk_bdev_aio_get_reqs_ctx *get_reqs;
};

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
 * @param ctx Ptr of ctx which is allocated outside.
 * @param bt bdev_target
 * @returns 0 on success. On error: returns -1
 */
int spdk_bdev_aio_ctx_setup(struct spdk_bdev_aio_ctx *ctx, struct spdk_bdev_target *bt);

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
 * @param nr_min minimum number of reqs. if -1, then all reqs should be completed.
 * @param nr maxinum number of reqs
 * @param reqs array to contain completed reqs.
 * @param timeout Waiting time (Not implemented)
 * @returns On error: returns -errno; On success: returns number of gotten reqs.
 */
int spdk_bdev_aio_ctx_get_reqs(struct spdk_bdev_aio_ctx *ctx,
		int nr_min, int nr, struct spdk_bdev_aio_req *reqs[],
		struct timespec *timeout);

/**
 * Destroy the context.
 *
 * @note
 * Cancel all outstanding asynchronous I/O operations against ctx, and will destroy the ctx.
 * Note cancel is not implemented.
 *
 * @param ctx Ptr of ctx which is allocated outside.
 * @returns On error: returns -errno; On success: returns number of gotten reqs.
 */
int spdk_bdev_aio_ctx_destroy(struct spdk_bdev_aio_ctx *ctx);

/**
 * Submit reqs to context.
 *
 * @note
 * Queues nr I/O requests for processing in the AIO context ctx
 *
 * @param ctx Ptr of ctx which is allocated outside.
 * @param nr Number of reqs
 * @param reqs Array to req pointers.
 * @returns On error: returns -errno; On success: returns number of queued reqs.
 */
int spdk_bdev_aio_ctx_submit(struct spdk_bdev_aio_ctx *ctx,
		int nr, struct spdk_bdev_aio_req *reqs[]);

/**
 * Prepare one req with NVMe admin passthru command type.
 *
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer.
 * @param pin_buf Data buffer related to admin command, it should be DMAable
 * @param data_len Length of data buffer
 */
void spdk_bdev_aio_req_prep_admin_passthru(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_cmd *cmd, void *pin_buf, size_t data_len);

/**
 * Prepare one req with NVMe IO passthru command type.
 *
 * @param req Ptr of req.
 * @param cmd Ptr of NVMe cmd buffer.
 * @param pin_buf Data buffer related to IO command, it should be DMAable
 * @param data_len Length of data buffer
 * @param pin_meta Metadata buffer related to IO command, it should be DMAable
 * @param md_len Length of metadata buffer
 */
void spdk_bdev_aio_req_prep_io_passthru(struct spdk_bdev_aio_req *req, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len);

/**
 * Set up one specific callback function and its argument to req.
 *
 * @param req Ptr of req.
 * @param cb Specific callback function.
 * @param cb_arg Argument for callback function.
 */
void spdk_bdev_aio_req_set_cb(struct spdk_bdev_aio_req *req, spdk_bdev_aio_req_complete_cb cb, void *cb_arg);

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
int spdk_bdev_aio_req_admin_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
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
int spdk_bdev_aio_req_io_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len, struct spdk_bdev_ret *ret);

#ifdef __cplusplus
}
#endif

#endif // SPDK_BDEV_TARGET_H
