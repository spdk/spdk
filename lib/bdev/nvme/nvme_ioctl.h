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

#ifndef LIB_BDEV_NVME_IOCTL_H
#define LIB_BDEV_NVME_IOCTL_H

#include "spdk/stdinc.h"

#include "spdk/nvme.h"

struct nvme_ctrlr;
struct nvme_bdev;

enum ioctl_conn_type_t {
	IOCTL_CONN_TYPE_CHAR,
	IOCTL_CONN_TYPE_BLK,
};

struct spdk_nvme_ioctl_conn {
	int			connfd;
	enum ioctl_conn_type_t  type;
	/*
	 * nvme_ctrlr or nvme_bdev based on type
	 */
	void			*device;
	void			*epoll_event_dataptr;

	TAILQ_ENTRY(spdk_nvme_ioctl_conn) conn_tailq;
};

int spdk_nvme_ioctl_init(void);
void spdk_nvme_ioctl_fini(void);

int spdk_nvme_ctrlr_create_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr);
void spdk_nvme_ctrlr_delete_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr);

#endif /* LIB_BDEV_NVME_IOCTL_H */
