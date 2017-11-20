/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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

#ifndef _VHOST_NET_USER_H
#define _VHOST_NET_USER_H

#include "spdk/stdinc.h"

#include "spdk_internal/log.h"

#include "../virtio_dev.h"

struct vhost_vring_state {
	unsigned int index;
	unsigned int num;
};

struct vhost_vring_file {
	unsigned int index;
	int fd;
};

struct vhost_vring_addr {
	unsigned int index;
	/* Option flags. */
	unsigned int flags;
	/* Flag values: */
	/* Whether log address is valid. If set enables logging. */
#define VHOST_VRING_F_LOG 0

	/* Start of array of descriptors (virtually contiguous) */
	uint64_t desc_user_addr;
	/* Used structure address. Must be 32 bit aligned */
	uint64_t used_user_addr;
	/* Available structure address. Must be 16 bit aligned */
	uint64_t avail_user_addr;
	/* Logging support. */
	/* Log writes to used structure, at offset calculated from specified
	 * address. Address must be 32 bit aligned.
	 */
	uint64_t log_guest_addr;
};

enum vhost_user_request {
	VHOST_USER_NONE = 0,
	VHOST_USER_GET_FEATURES = 1,
	VHOST_USER_SET_FEATURES = 2,
	VHOST_USER_SET_OWNER = 3,
	VHOST_USER_RESET_OWNER = 4,
	VHOST_USER_SET_MEM_TABLE = 5,
	VHOST_USER_SET_LOG_BASE = 6,
	VHOST_USER_SET_LOG_FD = 7,
	VHOST_USER_SET_VRING_NUM = 8,
	VHOST_USER_SET_VRING_ADDR = 9,
	VHOST_USER_SET_VRING_BASE = 10,
	VHOST_USER_GET_VRING_BASE = 11,
	VHOST_USER_SET_VRING_KICK = 12,
	VHOST_USER_SET_VRING_CALL = 13,
	VHOST_USER_SET_VRING_ERR = 14,
	VHOST_USER_GET_PROTOCOL_FEATURES = 15,
	VHOST_USER_SET_PROTOCOL_FEATURES = 16,
	VHOST_USER_GET_QUEUE_NUM = 17,
	VHOST_USER_SET_VRING_ENABLE = 18,
	VHOST_USER_MAX
};

extern const char *const vhost_msg_strings[VHOST_USER_MAX];

struct vhost_memory_region {
	uint64_t guest_phys_addr;
	uint64_t memory_size; /* bytes */
	uint64_t userspace_addr;
	uint64_t mmap_offset;
};

struct virtio_user_dev;

struct virtio_user_backend_ops {
	int (*setup)(struct virtio_user_dev *dev);
	int (*send_request)(struct virtio_user_dev *dev,
			    enum vhost_user_request req,
			    void *arg);
};

extern struct virtio_user_backend_ops ops_user;
extern struct virtio_user_backend_ops ops_kernel;

#endif
