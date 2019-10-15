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

#define FUSE_USE_VERSION 31

#include <fuse/cuse_lowlevel.h>

#include <linux/nvme_ioctl.h>
#include <linux/fs.h>

#include "nvme_internal.h"
#include "nvme_io_msg.h"

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

struct cuse_io_ctx {
	struct spdk_nvme_cmd	nvme_cmd;

	uint64_t		lba;
	uint32_t		lba_count;

	void			*data;
	int			data_len;

	fuse_req_t		req;
};

static void
cuse_io_ctx_free(struct cuse_io_ctx *ctx)
{
	spdk_free(ctx->data);
	free(ctx);
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
	struct cuse_io_ctx *ctx = arg;
	struct iovec out_iov[2];

	out_iov[0].iov_base = &cpl->cdw0;
	out_iov[0].iov_len = sizeof(cpl->cdw0);
	if (ctx->data_len > 0) {
		out_iov[1].iov_base = ctx->data;
		out_iov[1].iov_len = ctx->data_len;
		fuse_reply_ioctl_iov(ctx->req, 0, out_iov, 2);
	} else {
		fuse_reply_ioctl_iov(ctx->req, 0, out_iov, 1);
	}

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_admin_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &ctx->nvme_cmd, ctx->data, ctx->data_len,
					   cuse_nvme_admin_done_cb, (void *)ctx);
	if (rc < 0) {
		fuse_reply_err(ctx->req, EINVAL);
	}
}

static void
nvme_admin_cmd(fuse_req_t req, int cmd, void *arg,
	       struct fuse_file_info *fi, unsigned flags,
	       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct nvme_admin_cmd *admin_cmd;
	struct iovec in_iov, out_iov[2];
	struct cuse_io_ctx *ctx;
	int rv;
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

		ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
		if (!ctx) {
			SPDK_ERRLOG("Cannot allocate memory for cuse_io_ctx\n");
			fuse_reply_err(req, ENOMEM);
			return;
		}

		ctx->req = req;

		memset(&ctx->nvme_cmd, 0, sizeof(ctx->nvme_cmd));
		ctx->nvme_cmd.opc = admin_cmd->opcode;
		ctx->nvme_cmd.nsid = admin_cmd->nsid;
		ctx->nvme_cmd.cdw10 = admin_cmd->cdw10;
		ctx->nvme_cmd.cdw11 = admin_cmd->cdw11;
		ctx->nvme_cmd.cdw12 = admin_cmd->cdw12;
		ctx->nvme_cmd.cdw13 = admin_cmd->cdw13;
		ctx->nvme_cmd.cdw14 = admin_cmd->cdw14;
		ctx->nvme_cmd.cdw15 = admin_cmd->cdw15;

		ctx->data_len = admin_cmd->data_len;
		if (ctx->data_len > 0) {
			ctx->data = spdk_malloc(ctx->data_len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			if (!ctx->data) {
				SPDK_ERRLOG("Cannot allocate memory for data\n");
				fuse_reply_err(req, ENOMEM);
				free(ctx);
				return;
			}
		}

		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		fuse_reply_err(req, EINVAL);
		return;
		break;
	}

	rv = spdk_nvme_io_msg_send(cuse_device->ctrlr, 0, cuse_nvme_admin_cb, ctx);
	if (rv) {
		SPDK_ERRLOG("Cannot send io msg to the controller\n");
		fuse_reply_err(req, -rv);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_reset_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	fuse_req_t req = arg;

	rc = spdk_nvme_ctrlr_reset(ctrlr);
	if (rc) {
		fuse_reply_err(req, rc);
		return;
	}

	fuse_reply_ioctl_iov(req, 0, NULL, 0);
}

static void
nvme_reset(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rv;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	if (cuse_device->nsid) {
		SPDK_ERRLOG("Namespace reset not supported\n");
		fuse_reply_err(req, EINVAL);
		return;
	}

	rv = spdk_nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_reset_cb, (void *)req);
	if (rv) {
		SPDK_ERRLOG("Cannot send reset\n");
		fuse_reply_err(req, EINVAL);
	}
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
		nvme_reset(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
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
	SPDK_NOTICELOG("fuse session for device %s created\n", cuse_device->dev_name);
	fuse_session_loop(cuse_device->session);

	cuse_lowlevel_teardown(cuse_device->session);
	pthread_exit(NULL);
}

/*****************************************************************************
 * CUSE devices management
 */

static int
cuse_nvme_ns_start(struct cuse_device *ctrlr_device, uint32_t nsid)
{
	struct cuse_device *ns_device;

	ns_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	if (!ns_device) {
		SPDK_ERRLOG("Cannot allocate momeory for ns_device.");
		return -ENOMEM;
	}

	ns_device->ctrlr = ctrlr_device->ctrlr;
	ns_device->ctrlr_device = ctrlr_device;
	ns_device->idx = nsid;
	ns_device->nsid = nsid;
	snprintf(ns_device->dev_name, sizeof(ns_device->dev_name), "DEVNAME=spdk/nvme%dn%d\n",
		 ctrlr_device->idx, ns_device->idx);

	if (pthread_create(&ns_device->tid, NULL, cuse_thread, ns_device)) {
		SPDK_ERRLOG("pthread_create failed\n");
		return -1;
	}

	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_device, tailq);
	return 0;
}

static void
cuse_nvme_ctrlr_stop(struct cuse_device *ctrlr_device)
{
	struct cuse_device *ns_device, *tmp;

	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		fuse_session_exit(ns_device->session);
		pthread_kill(ns_device->tid, SIGHUP);
		pthread_join(ns_device->tid, NULL);
		TAILQ_REMOVE(&ctrlr_device->ns_devices, ns_device, tailq);
	}

	fuse_session_exit(ctrlr_device->session);
	pthread_kill(ctrlr_device->tid, SIGHUP);
	pthread_join(ctrlr_device->tid, NULL);
	TAILQ_REMOVE(&g_ctrlr_ctx_head, ctrlr_device, tailq);
}

static int
spdk_nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nsid;
	struct cuse_device *ctrlr_device;

	if (!ctrlr->opts.enable_cuse_devices) {
		return 0;
	}

	SPDK_NOTICELOG("Creating cuse device for controller\n");

	ctrlr_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot allocate memory for ctrlr_device.");
		return -ENOMEM;
	}

	TAILQ_INIT(&ctrlr_device->ns_devices);
	ctrlr_device->ctrlr = ctrlr;
	ctrlr_device->idx = g_controllers_found++;
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name),
		 "DEVNAME=spdk/nvme%d\n", ctrlr_device->idx);

	if (pthread_create(&ctrlr_device->tid, NULL, cuse_thread, ctrlr_device)) {
		SPDK_ERRLOG("pthread_create failed\n");
		free(ctrlr_device);
		return -1;
	}
	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);

	/* Start all active namespaces */
	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		nsid = i + 1;
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		if (cuse_nvme_ns_start(ctrlr_device, nsid) < 0) {
			SPDK_ERRLOG("Cannot start CUSE namespace device.");
			cuse_nvme_ctrlr_stop(ctrlr_device);
			return -1;
		}
	}

	return 0;
}

static int
spdk_nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	if (!ctrlr->opts.enable_cuse_devices) {
		return 0;
	}

	TAILQ_FOREACH(ctrlr_device, &g_ctrlr_ctx_head, tailq) {
		if (ctrlr_device->ctrlr == ctrlr) {
			break;
		}
	}

	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		return -1;
	}

	cuse_nvme_ctrlr_stop(ctrlr_device);

	return 0;
}

static struct spdk_nvme_io_msg_producer cuse_nvme_io_msg_producer = {
	.name = "cuse",

	.ctrlr_start = spdk_nvme_cuse_start,
	.ctrlr_stop = spdk_nvme_cuse_stop,
};

SPDK_NVME_IO_MSG_REGISTER(cuse, &cuse_nvme_io_msg_producer);
