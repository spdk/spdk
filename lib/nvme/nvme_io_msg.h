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

/** \file
 * SPDK cuse
 */


#ifndef SPDK_NVME_IO_MSG_H_
#define SPDK_NVME_IO_MSG_H_

#include "spdk/env.h"
#include "spdk/thread.h"

struct spdk_nvme_io_msg;
typedef void (*spdk_nvme_io_msg_fn)(struct spdk_nvme_io_msg *io);

struct spdk_nvme_io_msg {
	struct spdk_nvme_ctrlr	*ctrlr;
	uint32_t		nsid;

	spdk_nvme_io_msg_fn	fn;
	void			*arg;

	struct spdk_nvme_cmd	nvme_cmd;
	struct nvme_user_io	*nvme_user_io;

	uint64_t		lba;
	uint32_t		lba_count;

	void			*data;
	int			data_len;

	struct spdk_io_channel	*io_channel;
	struct spdk_nvme_qpair	*qpair;

	void			*ctx;
};

void cuse_nvme_io_msg_free(struct spdk_nvme_io_msg *io);
struct spdk_nvme_io_msg *cuse_nvme_io_msg_alloc(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *ctx);

int spdk_nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr);
int spdk_nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr);

int spdk_nvme_io_msg_process(void);
int spdk_nvme_io_msg_send(struct spdk_nvme_io_msg *io, spdk_nvme_io_msg_fn fn, void *arg);

int spdk_nvme_io_msg_ctrlr_start(struct spdk_nvme_ctrlr *ctrlr);
int spdk_nvme_io_msg_ctrlr_stop(struct spdk_nvme_ctrlr *ctrlr);

struct spdk_nvme_io_msg_producer {
	const char *name;

	void (*init)(void);
	void (*fini)(void);

	int (*ctrlr_start)(struct spdk_nvme_ctrlr *ctrlr);
	int (*ctrlr_stop)(struct spdk_nvme_ctrlr *ctrlr);

	STAILQ_ENTRY(spdk_net_framework) link;
};

void spdk_nvme_io_msg_register(struct spdk_nvme_io_msg_producer *io_msg_producer);

#define SPDK_NVME_IO_MSG_REGISTER(name, io_msg_producer) \
static void __attribute__((constructor)) spdk_nvme_io_msg_register_##name(void) \
{ \
	spdk_nvme_io_msg_register(io_msg_producer); \
}

#endif /* SPDK_NVME_CUSE_H_ */
