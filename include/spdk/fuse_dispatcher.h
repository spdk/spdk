/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Operations on a FUSE fsdev dispatcher
 */

#ifndef SPDK_FUSE_DISPATCHER_H
#define SPDK_FUSE_DISPATCHER_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_fuse_dispatcher;

enum spdk_fuse_arch {
	SPDK_FUSE_ARCH_NATIVE,
	SPDK_FUSE_ARCH_X86,
	SPDK_FUSE_ARCH_X86_64,
	SPDK_FUSE_ARCH_ARM,
	SPDK_FUSE_ARCH_ARM64,
	_SPDK_FUSE_ARCH_LAST,
};

/**
 * FUSE fsdev dispatcher create completion callback.
 *
 * \param cb_arg Callback argument specified upon create operation.
 * \param disp FUSE fsdev dispatcher object. NULL if creation failed.
 */
typedef void (*spdk_fuse_dispatcher_create_cpl_cb)(void *cb_arg, struct spdk_fuse_dispatcher *disp);

/**
 * FUSE fsdev dispatcher submit completion callback.
 *
 * \param cb_arg Callback argument specified upon submit operation.
 * \param error 0 if the operation succeeded, a negative error code otherwise.
 */
typedef void (*spdk_fuse_dispatcher_submit_cpl_cb)(void *cb_arg, int error);

/**
 * FUSE fsdev dispatcher delete completion callback.
 *
 * \param cb_arg Callback argument specified upon delete operation.
 * \param error 0 if the operation succeeded, a negative error code otherwise.
 */
typedef void (*spdk_fuse_dispatcher_delete_cpl_cb)(void *cb_arg, int error);


/** Asynchronous event type */
enum spdk_fuse_dispatcher_event_type {
	SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE,
};

/**
 * FUSE fsdev dispatcher event callback.
 *
 * \param type Event type.
 * \param disp FUSE fsdev dispatcher object.
 * \param event_ctx Context for the filesystem device event.
 */
typedef void (*spdk_fuse_dispatcher_event_cb)(enum spdk_fuse_dispatcher_event_type type,
		struct spdk_fuse_dispatcher *disp, void *event_ctx);

/**
 * Create a FUSE fsdev dispatcher
 *
 * \param fsdev_name Name of the fsdev to work with.
 * \param event_cb Dispatcher event callback.
 * \param event_ctx Dispatcher event callback's context.
 * \param cb Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success, a negative error code otherwise.
 * On success, the callback will always be called (even if the request ultimately failed).
 */
int spdk_fuse_dispatcher_create(const char *fsdev_name, spdk_fuse_dispatcher_event_cb event_cb,
				void *event_ctx, spdk_fuse_dispatcher_create_cpl_cb cb, void *cb_arg);

/**
 * Set a FUSE request source's HW architecture.
 *
 * Unless this function is called explicitly, the arch set to SPDK_FUSE_ARCH_NATIVE.
 *
 * \param disp FUSE fsdev dispatcher object.
 * \param fuse_arch FUSE request source's HW architecture
 *
 * \return 0 on success or -EINVAL if the architecture is not supported
 */
int spdk_fuse_dispatcher_set_arch(struct spdk_fuse_dispatcher *disp, enum spdk_fuse_arch fuse_arch);

/**
 * Get underlying fsdev name
 *
 * \param disp FUSE fsdev dispatcher object.
 *
 * \return fsdev name
 */
const char *spdk_fuse_dispatcher_get_fsdev_name(struct spdk_fuse_dispatcher *disp);

/**
 * Obtain an I/O channel for the FUSE fsdev dispatcher object. I/O channels are
 * bound to threads, so the resulting I/O channel may only be used from the thread
 * it was originally obtained from.
 *
 * \param disp FUSE fsdev dispatcher object.
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_fuse_dispatcher_get_io_channel(struct spdk_fuse_dispatcher *disp);

/**
 * Submit FUSE request
 *
 * \param disp FUSE fsdev dispatcher object.
 * \param ch I/O channel obtained from the \p spdk_fuse_dispatcher_get_io_channel.
 * \param in_iov Input IO vectors array.
 * \param in_iovcnt Size of the input IO vectors array.
 * \param out_iov Output IO vectors array.
 * \param out_iovcnt Size of the output IO vectors array.
 * \param cb Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success. On success, the callback will always
 * be called (even if the request ultimately failed). Return
 * negated errno on failure, in which case the callback will not be called.
 *  -ENOBUFS - the request cannot be submitted due to a lack of the internal IO objects
 *  -EINVAL - the request cannot be submitted as some FUSE request data is incorrect
 */
int spdk_fuse_dispatcher_submit_request(struct spdk_fuse_dispatcher *disp,
					struct spdk_io_channel *ch,
					struct iovec *in_iov, int in_iovcnt,
					struct iovec *out_iov, int out_iovcnt,
					spdk_fuse_dispatcher_submit_cpl_cb cb, void *cb_arg);

/**
 * Delete a FUSE fsdev dispatcher
 *
 * \param disp FUSE fsdev dispatcher object.
 * \param cb Completion callback.
 * \param cb_arg Context to be passed to the completion callback.
 *
 * \return 0 on success, a negative error code otherwise.
 * On success, the callback will always be called (even if the request ultimately failed).
 */
int spdk_fuse_dispatcher_delete(struct spdk_fuse_dispatcher *disp,
				spdk_fuse_dispatcher_delete_cpl_cb cb, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FUSE_DISPATCHER_H */
