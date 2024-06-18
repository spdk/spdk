/*   SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SPDK_THREAD_INTERNAL_H_
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/tree.h"

/**
 * \brief Represents a per-thread channel for accessing an I/O device.
 *
 * An I/O device may be a physical entity (i.e. NVMe controller) or a software
 *  entity (i.e. a blobstore).
 *
 * This structure is not part of the API - all accesses should be done through
 *  spdk_io_channel function calls.
 */
struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct io_device		*dev;
	uint32_t			ref;
	uint32_t			destroy_ref;
	RB_ENTRY(spdk_io_channel)	node;
	spdk_io_channel_destroy_cb	destroy_cb;

	uint8_t				_padding[40];
	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

SPDK_STATIC_ASSERT(sizeof(struct spdk_io_channel) == SPDK_IO_CHANNEL_STRUCT_SIZE, "incorrect size");

#endif /* SPDK_THREAD_INTERNAL_H_ */
