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

#include "spdk/stdinc.h"

#define FUSE_USE_VERSION 31

#include <fuse/cuse_lowlevel.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>

#include <linux/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <linux/fs.h>

#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

#include "nvme_internal.h"
#include "nvme_cuse.h"

#include "spdk/thread.h"

struct cuse_device {
	char				dev_name[128];

	struct spdk_nvme_ctrlr		*ctrlr;		/**< NVMe controller */
	uint32_t			nsid;		/**< NVMe name space id, or 0 */

	uint32_t			idx;
	pthread_t			tid;
	struct fuse_session		*session;

	struct cuse_device		*ctrlr_device;
	TAILQ_HEAD(, cuse_device)	ns_devices;

	TAILQ_ENTRY(cuse_device)	tailq;
};

static TAILQ_HEAD(, cuse_device) g_ctrlr_ctx_head = TAILQ_HEAD_INITIALIZER(g_ctrlr_ctx_head);
static int g_controllers_found = 0;
static bool g_cuse_initialized = false;

struct spdk_nvme_io_msg;

typedef void (*spdk_nvme_io_msg_fn)(struct spdk_nvme_io_msg *io);

struct spdk_ring *g_nvme_io_msgs;
pthread_mutex_t g_cuse_io_requests_lock;

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

	fuse_req_t		req;

	struct spdk_io_channel *io_channel;
	struct spdk_nvme_qpair *qpair;
};

#define SPDK_CUSE_REQUESTS_PROCESS_SIZE 8

/**
 * Send message to IO queue.
 */
static int
spdk_nvme_io_msg_send(struct spdk_nvme_io_msg *io, spdk_nvme_io_msg_fn fn, void *arg)
{
	int rc;

	io->fn = fn;
	io->arg = arg;

	/* Protect requests ring against preemptive producers */
	pthread_mutex_lock(&g_cuse_io_requests_lock);

	rc = spdk_ring_enqueue(g_nvme_io_msgs, (void **)&io, 1, NULL);
	if (rc != 1) {
		assert(false);
		/* FIXIT! Do something with request here */
		return -ENOMEM;
	}

	pthread_mutex_unlock(&g_cuse_io_requests_lock);

	return 0;
}

/**
 * Get next IO message and process on the current SPDK thread.
 */
struct nvme_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;

	bool			collect_spin_stat;
	uint64_t		spin_ticks;
	uint64_t		start_ticks;
	uint64_t		end_ticks;
};

int
spdk_nvme_io_msg_process(void)
{
	int i;
	void *requests[SPDK_CUSE_REQUESTS_PROCESS_SIZE];
	int count;
	struct nvme_io_channel *ch;

	if (!g_cuse_initialized) {
		return 0;
	}

	count = spdk_ring_dequeue(g_nvme_io_msgs, requests, SPDK_CUSE_REQUESTS_PROCESS_SIZE);
	if (count == 0) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct spdk_nvme_io_msg *io = requests[i];

		assert(io != NULL);

		if (io->nsid != 0) {
			io->io_channel = spdk_get_io_channel(io->ctrlr);
			ch = spdk_io_channel_get_ctx(io->io_channel);
			io->qpair = ch->qpair;
		}

		io->fn(io);
	}

	return 0;
}

static void
cuse_nvme_io_msg_free(struct spdk_nvme_io_msg *io)
{
	if (io->io_channel) {
		spdk_put_io_channel(io->io_channel);
	}
	spdk_free(io->data);
	free(io);
}

static struct spdk_nvme_io_msg *
cuse_nvme_io_msg_alloc(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, fuse_req_t req)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)calloc(1, sizeof(struct spdk_nvme_io_msg));

	io->ctrlr = ctrlr;
	io->nsid = nsid;
	io->req = req;
	return io;
}

#define FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, val)		\
	if (out_bufsz == 0) {						\
		struct iovec out_iov;					\
		out_iov.iov_base = (void *)arg;				\
		out_iov.iov_len = sizeof(val);				\
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);	\
		return;							\
	}

static void
cuse_nvme_admin_done_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_io_msg *ctx = arg;
	struct iovec out_iov[2];

	out_iov[0].iov_base = &cpl->cdw0;
	out_iov[0].iov_len = sizeof(cpl->cdw0);
	if (ctx->data_len > 0) {
		out_iov[1].iov_base = ctx->data;
		out_iov[1].iov_len = ctx->data_len;
		fuse_reply_ioctl_iov(ctx->req, 0, out_iov, 2);
		spdk_free(ctx->data);
	} else {
		fuse_reply_ioctl_iov(ctx->req, 0, out_iov, 1);
	}

	free(ctx);
}

static void
cuse_nvme_admin_cb(struct spdk_nvme_io_msg *io)
{
	int rc;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(io->ctrlr, &io->nvme_cmd, io->data, io->data_len,
					   cuse_nvme_admin_done_cb, (void *)io);
	if (rc < 0) {
		fuse_reply_err(io->req, EINVAL);
	}
}

static void
nvme_admin_cmd(fuse_req_t req, int cmd, void *arg,
	       struct fuse_file_info *fi, unsigned flags,
	       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct nvme_admin_cmd *admin_cmd;
	struct iovec in_iov, out_iov[2];
	struct spdk_nvme_io_msg *io;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, in_bufsz, *admin_cmd);

	admin_cmd = (struct nvme_admin_cmd *)in_buf;

	switch (spdk_nvme_opc_get_data_transfer(admin_cmd->opcode)) {
	case SPDK_NVME_DATA_NONE:
		SPDK_ERRLOG("SPDK_NVME_DATA_NONE not implemented\n");
		fuse_reply_err(req, EINVAL);
		return;
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		SPDK_ERRLOG("SPDK_NVME_DATA_HOST_TO_CONTROLLER not implemented\n");
		fuse_reply_err(req, EINVAL);
		return;
	case SPDK_NVME_DATA_CONTROLLER_TO_HOST:
		if (out_bufsz == 0) {
			out_iov[0].iov_base = &((struct nvme_admin_cmd *)arg)->result;
			out_iov[0].iov_len = sizeof(uint32_t);
			if (admin_cmd->data_len > 0) {
				out_iov[1].iov_base = (void *)admin_cmd->addr;
				out_iov[1].iov_len = admin_cmd->data_len;
				fuse_reply_ioctl_retry(req, &in_iov, 1, out_iov, 2);
			} else {
				fuse_reply_ioctl_retry(req, &in_iov, 1, out_iov, 1);
			}
			return;
		}

		io = cuse_nvme_io_msg_alloc(cuse_device->ctrlr, 0, req);

		memset(&io->nvme_cmd, 0, sizeof(io->nvme_cmd));
		io->nvme_cmd.opc = admin_cmd->opcode;
		io->nvme_cmd.nsid = admin_cmd->nsid;
		io->nvme_cmd.cdw10 = admin_cmd->cdw10;
		io->nvme_cmd.cdw11 = admin_cmd->cdw11;
		io->nvme_cmd.cdw12 = admin_cmd->cdw12;
		io->nvme_cmd.cdw13 = admin_cmd->cdw13;
		io->nvme_cmd.cdw14 = admin_cmd->cdw14;
		io->nvme_cmd.cdw15 = admin_cmd->cdw15;

		io->req = req;
		io->data_len = admin_cmd->data_len;
		if (io->data_len > 0) {
			io->data = spdk_malloc(io->data_len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		}

		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		fuse_reply_err(req, EINVAL);
		return;
		break;
	}

	spdk_nvme_io_msg_send(io, cuse_nvme_admin_cb, NULL);
}

static void
cuse_ioctl(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		nvme_admin_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESET:
		fuse_reply_err(req, EINVAL);
		break;

	case NVME_IOCTL_IO_CMD:
		fuse_reply_err(req, EINVAL);
		break;

	default:
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

/*****************************************************************************
 * CUSE threads initialization.
 */

static void cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

static const struct cuse_lowlevel_ops cuse_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ioctl,
};

static void *
cuse_thread(void *arg)
{
	struct cuse_device *cuse_device = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	const char *dev_info_argv[] = { cuse_device->dev_name };
	struct cuse_info ci;
	int multithreaded;

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	cuse_device->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_clop,
			       &multithreaded, cuse_device);
	fuse_session_loop(cuse_device->session);

	pthread_exit(NULL);
}

/*****************************************************************************
 * CUSE devices management
 */

int
spdk_nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nsid;
	struct cuse_device *ctrlr_device;
	struct cuse_device *ns_device;

	if (!ctrlr->opts.enable_cuse_devices) {
		return 0;
	}

	if (!g_nvme_io_msgs) {
		g_nvme_io_msgs = spdk_ring_create(SPDK_RING_TYPE_MP_MC, 65536, SPDK_ENV_SOCKET_ID_ANY);
		if (!g_nvme_io_msgs) {
			SPDK_ERRLOG("Unable to allocate memory for message ring\n");
			return -ENOMEM;
		}
	}

	ctrlr_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	ctrlr_device->ctrlr = ctrlr;
	ctrlr_device->idx = g_controllers_found++;
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name),
		 "DEVNAME=nvme%d\n", ctrlr_device->idx);

	if (pthread_create(&ctrlr_device->tid, NULL, cuse_thread, ctrlr_device)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}

	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		nsid = i + 1;
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		ns_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
		ns_device->ctrlr = ctrlr;
		ns_device->ctrlr_device = ctrlr_device;
		ns_device->idx = nsid;
		ns_device->nsid = nsid;
		snprintf(ns_device->dev_name, sizeof(ns_device->dev_name), "DEVNAME=nvme%dn%d\n", ctrlr_device->idx,
			 ns_device->idx);

		if (pthread_create(&ns_device->tid, NULL, cuse_thread, ns_device)) {
			SPDK_ERRLOG("pthread_create failed\n");
		}
	}

	g_cuse_initialized = true;

	return 0;
}

int
spdk_nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;
	struct cuse_device *ns_device, *tmp;

	TAILQ_FOREACH(ctrlr_device, &g_ctrlr_ctx_head, tailq) {
		if (ctrlr_device->ctrlr == ctrlr) {
			break;
		}
	}

	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		return -1;
	}

	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		fuse_session_exit(ns_device->session);
		pthread_kill(ns_device->tid, SIGHUP);
		pthread_join(ns_device->tid, NULL);
		TAILQ_REMOVE(&ctrlr_device->ns_devices, ns_device, tailq);
	}

	fuse_session_exit(ctrlr_device->session);
	pthread_kill(ctrlr_device->tid, SIGHUP);
	pthread_join(ctrlr_device->tid, NULL);

	return 0;
}
