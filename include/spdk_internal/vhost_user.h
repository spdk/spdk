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

/** \file
 * Structures defined in the vhost-user specification
 */

#ifndef SPDK_VHOST_USER_H
#define SPDK_VHOST_USER_H

#include "spdk/stdinc.h"

#include <linux/vhost.h>

#ifndef VHOST_USER_MEMORY_MAX_NREGIONS
#define VHOST_USER_MEMORY_MAX_NREGIONS	8
#endif

#ifndef VHOST_USER_MAX_CONFIG_SIZE
#define VHOST_USER_MAX_CONFIG_SIZE	256
#endif

#ifndef VHOST_USER_PROTOCOL_F_MQ
#define VHOST_USER_PROTOCOL_F_MQ	0
#endif

#ifndef VHOST_USER_PROTOCOL_F_CONFIG
#define VHOST_USER_PROTOCOL_F_CONFIG	9
#endif

#ifndef VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD
#define VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD 12
#endif

#ifndef VHOST_USER_F_PROTOCOL_FEATURES
#define VHOST_USER_F_PROTOCOL_FEATURES	30
#endif

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
	VHOST_USER_SEND_RARP = 19,
	VHOST_USER_NET_SET_MTU = 20,
	VHOST_USER_SET_SLAVE_REQ_FD = 21,
	VHOST_USER_IOTLB_MSG = 22,
	VHOST_USER_GET_CONFIG = 24,
	VHOST_USER_SET_CONFIG = 25,
	VHOST_USER_CRYPTO_CREATE_SESS = 26,
	VHOST_USER_CRYPTO_CLOSE_SESS = 27,
	VHOST_USER_POSTCOPY_ADVISE = 28,
	VHOST_USER_POSTCOPY_LISTEN = 29,
	VHOST_USER_POSTCOPY_END = 30,
	VHOST_USER_MAX
};

/** Get/set config msg payload */
struct vhost_user_config {
	uint32_t offset;
	uint32_t size;
	uint32_t flags;
	uint8_t region[VHOST_USER_MAX_CONFIG_SIZE];
};

/** Fixed-size vhost_memory struct */
struct vhost_memory_padded {
	uint32_t nregions;
	uint32_t padding;
	struct vhost_memory_region regions[VHOST_USER_MEMORY_MAX_NREGIONS];
};

struct vhost_user_msg {
	enum vhost_user_request request;

#define VHOST_USER_VERSION_MASK     0x3
#define VHOST_USER_REPLY_MASK       (0x1 << 2)
	uint32_t flags;
	uint32_t size; /**< the following payload size */
	union {
#define VHOST_USER_VRING_IDX_MASK   0xff
#define VHOST_USER_VRING_NOFD_MASK  (0x1 << 8)
		uint64_t u64;
		struct vhost_vring_state state;
		struct vhost_vring_addr addr;
		struct vhost_memory_padded memory;
		struct vhost_user_config cfg;
	} payload;
} __attribute((packed));

#define VHOST_USER_HDR_SIZE offsetof(struct vhost_user_msg, payload.u64)
#define VHOST_USER_PAYLOAD_SIZE \
	(sizeof(struct vhost_user_msg) - VHOST_USER_HDR_SIZE)

#endif /* SPDK_VHOST_USER_H */
