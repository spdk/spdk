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

/*
 * Head of ioctl req/resp includes magic, ioctl_cmd, handle and total_len.
 */
#define IOCTL_HEAD_SIZE (sizeof(uint32_t) * 4)

/*
 * These are sent over Unix domain socket in the request/response magic fields.
 */
#define IOCTL_REQ_MAGIC		0x58444F4E
#define IOCTL_RESP_MAGIC	0x58464549

struct nvme_ctrlr;
struct nvme_bdev;

struct spdk_nvme_ioctl_req {
	uint32_t	req_magic;
	uint32_t	ioctl_cmd;
	uint32_t	handle;
	uint32_t	total_len;

	char		*cmd_buf;
	char		*data;
	char		*metadata;
	uint32_t	cmd_len;
	uint32_t	data_len;
	uint32_t	md_len;
};

struct spdk_nvme_ioctl_resp {
	uint32_t	resp_magic;
	uint32_t	ioctl_cmd;
	uint32_t	handle;
	uint32_t	total_len;

	/*
	 * If ioctl_ret is 0, that means cmd is executed succesfully;
	 * If (int)ioctl_ret is >0, ioctl_ret represents CQE status;
	 * If (int)ioctl_ret is <0, ioctl_ret means cmd is not executed due to some error.
	 */
	uint32_t	ioctl_ret;

	char		*cmd_buf;
	char		*data;
	char		*metadata;
	uint32_t	cmd_len;
	uint32_t	data_len;
	uint32_t	md_len;
};

enum ioctl_conn_state_t {
	IOCTL_CONN_STATE_RECV_HEAD,
	IOCTL_CONN_STATE_RECV_CMD,
	IOCTL_CONN_STATE_RECV_DATA,
	IOCTL_CONN_STATE_RECV_METADATA,
	IOCTL_CONN_STATE_PROC,
	IOCTL_CONN_STATE_XMIT_HEAD,
	IOCTL_CONN_STATE_XMIT_RET, // return value of ioctl
	IOCTL_CONN_STATE_XMIT_CMD,
	IOCTL_CONN_STATE_XMIT_DATA,
	IOCTL_CONN_STATE_XMIT_METADATA,
	IOCTL_CONN_STATE_CLOSE, // indicate ioctl_conn should be closed
};

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

	enum ioctl_conn_state_t		state;
	uint32_t			offset;
	struct spdk_nvme_ioctl_req	req;
	struct spdk_nvme_ioctl_resp	resp;

	TAILQ_ENTRY(spdk_nvme_ioctl_conn) conn_tailq;
};

int spdk_nvme_ioctl_init(void);
void spdk_nvme_ioctl_fini(void);

int spdk_nvme_ctrlr_create_pci_symlink(struct nvme_ctrlr *nvme_ctrlr);
void spdk_nvme_ctrlr_delete_pci_symlink(struct nvme_ctrlr *nvme_ctrlr);
int spdk_nvme_ctrlr_create_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr);
void spdk_nvme_ctrlr_delete_ioctl_sockfd(struct nvme_ctrlr *nvme_ctrlr);
int spdk_nvme_bdev_create_ioctl_sockfd(struct nvme_bdev *bdev, int ns_id);
void spdk_nvme_bdev_delete_ioctl_sockfd(struct nvme_bdev *bdev);

int spdk_nvme_ioctl_conn_recv(struct spdk_nvme_ioctl_conn *ioctl_conn);
int spdk_nvme_ioctl_conn_xmit(struct spdk_nvme_ioctl_conn *ioctl_conn);
void spdk_nvme_ioctl_conn_free(struct spdk_nvme_ioctl_conn *ioctl_conn);

int spdk_nvme_ioctl_proc(struct spdk_nvme_ioctl_conn *ioctl_conn);

#endif /* LIB_BDEV_NVME_IOCTL_H */
