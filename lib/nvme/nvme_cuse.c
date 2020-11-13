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

#include <fuse3/cuse_lowlevel.h>

#include <linux/nvme_ioctl.h>
#include <linux/fs.h>

#include "nvme_internal.h"
#include "nvme_io_msg.h"
#include "nvme_cuse.h"

struct cuse_device {
	bool				is_started;

	char				dev_name[128];
	uint32_t			index;
	int				claim_fd;
	char				lock_name[64];

	struct spdk_nvme_ctrlr		*ctrlr;		/**< NVMe controller */
	uint32_t			nsid;		/**< NVMe name space id, or 0 */

	pthread_t			tid;
	struct fuse_session		*session;

	struct cuse_device		*ctrlr_device;
	struct cuse_device		*ns_devices;	/**< Array of cuse ns devices */

	TAILQ_ENTRY(cuse_device)	tailq;
};

static pthread_mutex_t g_cuse_mtx = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, cuse_device) g_ctrlr_ctx_head = TAILQ_HEAD_INITIALIZER(g_ctrlr_ctx_head);
static struct spdk_bit_array *g_ctrlr_started;

struct cuse_io_ctx {
	struct spdk_nvme_cmd		nvme_cmd;
	enum spdk_nvme_data_transfer	data_transfer;

	uint64_t			lba;
	uint32_t			lba_count;

	void				*data;
	int				data_len;

	fuse_req_t			req;
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
cuse_nvme_admin_cmd_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = arg;
	struct iovec out_iov[2];
	struct spdk_nvme_cpl _cpl;

	if (ctx->data_transfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER ||
	    ctx->data_transfer == SPDK_NVME_DATA_NONE) {
		fuse_reply_ioctl_iov(ctx->req, cpl->status.sc, NULL, 0);
	} else {
		memcpy(&_cpl, cpl, sizeof(struct spdk_nvme_cpl));

		out_iov[0].iov_base = &_cpl.cdw0;
		out_iov[0].iov_len = sizeof(_cpl.cdw0);

		if (ctx->data_len > 0) {
			out_iov[1].iov_base = ctx->data;
			out_iov[1].iov_len = ctx->data_len;
			fuse_reply_ioctl_iov(ctx->req, cpl->status.sc, out_iov, 2);
		} else {
			fuse_reply_ioctl_iov(ctx->req, cpl->status.sc, out_iov, 1);
		}
	}

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_admin_cmd_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &ctx->nvme_cmd, ctx->data, ctx->data_len,
					   cuse_nvme_admin_cmd_cb, (void *)ctx);
	if (rc < 0) {
		fuse_reply_err(ctx->req, EINVAL);
		cuse_io_ctx_free(ctx);
	}
}

static void
cuse_nvme_admin_cmd_send(fuse_req_t req, struct nvme_admin_cmd *admin_cmd,
			 const void *data)
{
	struct cuse_io_ctx *ctx;
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	int rv;

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for cuse_io_ctx\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ctx->req = req;
	ctx->data_transfer = spdk_nvme_opc_get_data_transfer(admin_cmd->opcode);

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
		if (data != NULL) {
			memcpy(ctx->data, data, ctx->data_len);
		}
	}

	rv = nvme_io_msg_send(cuse_device->ctrlr, 0, cuse_nvme_admin_cmd_execute, ctx);
	if (rv) {
		SPDK_ERRLOG("Cannot send io msg to the controller\n");
		fuse_reply_err(req, -rv);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_admin_cmd(fuse_req_t req, int cmd, void *arg,
		    struct fuse_file_info *fi, unsigned flags,
		    const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct nvme_admin_cmd *admin_cmd;
	struct iovec in_iov[2], out_iov[2];

	in_iov[0].iov_base = (void *)arg;
	in_iov[0].iov_len = sizeof(*admin_cmd);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);
		return;
	}

	admin_cmd = (struct nvme_admin_cmd *)in_buf;

	switch (spdk_nvme_opc_get_data_transfer(admin_cmd->opcode)) {
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		if (admin_cmd->addr != 0) {
			in_iov[1].iov_base = (void *)admin_cmd->addr;
			in_iov[1].iov_len = admin_cmd->data_len;
			if (in_bufsz == sizeof(*admin_cmd)) {
				fuse_reply_ioctl_retry(req, in_iov, 2, NULL, 0);
				return;
			}
			cuse_nvme_admin_cmd_send(req, admin_cmd, in_buf + sizeof(*admin_cmd));
		} else {
			cuse_nvme_admin_cmd_send(req, admin_cmd, NULL);
		}
		return;
	case SPDK_NVME_DATA_NONE:
	case SPDK_NVME_DATA_CONTROLLER_TO_HOST:
		if (out_bufsz == 0) {
			out_iov[0].iov_base = &((struct nvme_admin_cmd *)arg)->result;
			out_iov[0].iov_len = sizeof(uint32_t);
			if (admin_cmd->data_len > 0) {
				out_iov[1].iov_base = (void *)admin_cmd->addr;
				out_iov[1].iov_len = admin_cmd->data_len;
				fuse_reply_ioctl_retry(req, in_iov, 1, out_iov, 2);
			} else {
				fuse_reply_ioctl_retry(req, in_iov, 1, out_iov, 1);
			}
			return;
		}

		cuse_nvme_admin_cmd_send(req, admin_cmd, NULL);

		return;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		fuse_reply_err(req, EINVAL);
		return;
	}
}

static void
cuse_nvme_reset_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
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
cuse_nvme_reset(fuse_req_t req, int cmd, void *arg,
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

	rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_reset_execute, (void *)req);
	if (rv) {
		SPDK_ERRLOG("Cannot send reset\n");
		fuse_reply_err(req, EINVAL);
	}
}

/*****************************************************************************
 * Namespace IO requests
 */

static void
cuse_nvme_submit_io_write_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = (struct cuse_io_ctx *)ref;

	fuse_reply_ioctl_iov(ctx->req, cpl->status.sc, NULL, 0);

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_submit_io_write_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	rc = spdk_nvme_ns_cmd_write(ns, ctrlr->external_io_msgs_qpair, ctx->data,
				    ctx->lba, /* LBA start */
				    ctx->lba_count, /* number of LBAs */
				    cuse_nvme_submit_io_write_done, ctx, 0);

	if (rc != 0) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_submit_io_write(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags, uint32_t block_size,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io = in_buf;
	struct cuse_io_ctx *ctx;
	int rc;

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for context\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ctx->req = req;
	ctx->lba = user_io->slba;
	ctx->lba_count = user_io->nblocks + 1;
	ctx->data_len = ctx->lba_count * block_size;

	ctx->data = spdk_zmalloc(ctx->data_len, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (ctx->data == NULL) {
		SPDK_ERRLOG("Write buffer allocation failed\n");
		fuse_reply_err(ctx->req, ENOMEM);
		free(ctx);
		return;
	}

	memcpy(ctx->data, in_buf + sizeof(*user_io), ctx->data_len);

	rc = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_submit_io_write_cb,
			      ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot send write io\n");
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
	}
}

static void
cuse_nvme_submit_io_read_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = (struct cuse_io_ctx *)ref;
	struct iovec iov;

	iov.iov_base = ctx->data;
	iov.iov_len = ctx->data_len;

	fuse_reply_ioctl_iov(ctx->req, cpl->status.sc, &iov, 1);

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_submit_io_read_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	rc = spdk_nvme_ns_cmd_read(ns, ctrlr->external_io_msgs_qpair, ctx->data,
				   ctx->lba, /* LBA start */
				   ctx->lba_count, /* number of LBAs */
				   cuse_nvme_submit_io_read_done, ctx, 0);

	if (rc != 0) {
		SPDK_ERRLOG("read failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_submit_io_read(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned flags, uint32_t block_size,
			 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rc;
	struct cuse_io_ctx *ctx;
	const struct nvme_user_io *user_io = in_buf;

	ctx = (struct cuse_io_ctx *)calloc(1, sizeof(struct cuse_io_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Cannot allocate memory for context\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ctx->req = req;
	ctx->lba = user_io->slba;
	ctx->lba_count = user_io->nblocks + 1;

	ctx->data_len = ctx->lba_count * block_size;
	ctx->data = spdk_zmalloc(ctx->data_len, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (ctx->data == NULL) {
		SPDK_ERRLOG("Read buffer allocation failed\n");
		fuse_reply_err(ctx->req, ENOMEM);
		free(ctx);
		return;
	}

	rc = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_submit_io_read_cb, ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot send read io\n");
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
	}
}


static void
cuse_nvme_submit_io(fuse_req_t req, int cmd, void *arg,
		    struct fuse_file_info *fi, unsigned flags,
		    const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io;
	struct iovec in_iov[2], out_iov;
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	struct spdk_nvme_ns *ns;
	uint32_t block_size;

	in_iov[0].iov_base = (void *)arg;
	in_iov[0].iov_len = sizeof(*user_io);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);
		return;
	}

	user_io = in_buf;

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	block_size = spdk_nvme_ns_get_sector_size(ns);

	switch (user_io->opcode) {
	case SPDK_NVME_OPC_READ:
		out_iov.iov_base = (void *)user_io->addr;
		out_iov.iov_len = (user_io->nblocks + 1) * block_size;
		if (out_bufsz == 0) {
			fuse_reply_ioctl_retry(req, in_iov, 1, &out_iov, 1);
			return;
		}

		cuse_nvme_submit_io_read(cuse_device, req, cmd, arg, fi, flags,
					 block_size, in_buf, in_bufsz, out_bufsz);
		break;
	case SPDK_NVME_OPC_WRITE:
		in_iov[1].iov_base = (void *)user_io->addr;
		in_iov[1].iov_len = (user_io->nblocks + 1) * block_size;
		if (in_bufsz == sizeof(*user_io)) {
			fuse_reply_ioctl_retry(req, in_iov, 2, NULL, 0);
			return;
		}

		cuse_nvme_submit_io_write(cuse_device, req, cmd, arg, fi, flags,
					  block_size, in_buf, in_bufsz, out_bufsz);
		break;
	default:
		SPDK_ERRLOG("SUBMIT_IO: opc:%d not valid\n", user_io->opcode);
		fuse_reply_err(req, EINVAL);
		return;
	}

}

/*****************************************************************************
 * Other namespace IOCTLs
 */
static void
cuse_blkgetsize64(fuse_req_t req, int cmd, void *arg,
		  struct fuse_file_info *fi, unsigned flags,
		  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	uint64_t size;
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, size);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	size = spdk_nvme_ns_get_num_sectors(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void
cuse_blkpbszget(fuse_req_t req, int cmd, void *arg,
		struct fuse_file_info *fi, unsigned flags,
		const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int pbsz;
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, pbsz);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	pbsz = spdk_nvme_ns_get_sector_size(ns);
	fuse_reply_ioctl(req, 0, &pbsz, sizeof(pbsz));
}

static void
cuse_blkgetsize(fuse_req_t req, int cmd, void *arg,
		struct fuse_file_info *fi, unsigned flags,
		const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	long size;
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, size);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);

	/* return size in 512 bytes blocks */
	size = spdk_nvme_ns_get_num_sectors(ns) * 512 / spdk_nvme_ns_get_sector_size(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void
cuse_getid(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	fuse_reply_ioctl(req, cuse_device->nsid, NULL, 0);
}

static void
cuse_ctrlr_ioctl(fuse_req_t req, int cmd, void *arg,
		 struct fuse_file_info *fi, unsigned flags,
		 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch ((unsigned int)cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		cuse_nvme_admin_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESET:
		cuse_nvme_reset(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

static void
cuse_ns_ioctl(fuse_req_t req, int cmd, void *arg,
	      struct fuse_file_info *fi, unsigned flags,
	      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch ((unsigned int)cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		cuse_nvme_admin_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_SUBMIT_IO:
		cuse_nvme_submit_io(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		cuse_getid(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKPBSZGET:
		cuse_blkpbszget(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE:
		/* Returns the device size as a number of 512-byte blocks (returns pointer to long) */
		cuse_blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE64:
		/* Returns the device size in sectors (returns pointer to uint64_t) */
		cuse_blkgetsize64(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
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

static const struct cuse_lowlevel_ops cuse_ctrlr_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ctrlr_ioctl,
};

static const struct cuse_lowlevel_ops cuse_ns_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ns_ioctl,
};

static void *
cuse_thread(void *arg)
{
	struct cuse_device *cuse_device = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	char devname_arg[128 + 8];
	const char *dev_info_argv[] = { devname_arg };
	struct cuse_info ci;
	int multithreaded;
	int rc;
	struct fuse_buf buf = { .mem = NULL };
	struct pollfd fds;
	int timeout_msecs = 500;

	spdk_unaffinitize_thread();

	snprintf(devname_arg, sizeof(devname_arg), "DEVNAME=%s", cuse_device->dev_name);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	if (cuse_device->nsid) {
		cuse_device->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ns_clop,
				       &multithreaded, cuse_device);
	} else {
		cuse_device->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ctrlr_clop,
				       &multithreaded, cuse_device);
	}
	if (!cuse_device->session) {
		SPDK_ERRLOG("Cannot create cuse session\n");
		goto err;
	}

	SPDK_NOTICELOG("fuse session for device %s created\n", cuse_device->dev_name);

	/* Receive and process fuse requests */
	fds.fd = fuse_session_fd(cuse_device->session);
	fds.events = POLLIN;
	while (!fuse_session_exited(cuse_device->session)) {
		rc = poll(&fds, 1, timeout_msecs);
		if (rc <= 0) {
			continue;
		}
		rc = fuse_session_receive_buf(cuse_device->session, &buf);
		if (rc > 0) {
			fuse_session_process_buf(cuse_device->session, &buf);
		}
	}
	free(buf.mem);
	fuse_session_reset(cuse_device->session);
	cuse_lowlevel_teardown(cuse_device->session);
err:
	pthread_exit(NULL);
}

/*****************************************************************************
 * CUSE devices management
 */

static int
cuse_nvme_ns_start(struct cuse_device *ctrlr_device, uint32_t nsid)
{
	struct cuse_device *ns_device;
	int rv;

	ns_device = &ctrlr_device->ns_devices[nsid - 1];
	if (ns_device->is_started) {
		return 0;
	}

	ns_device->ctrlr = ctrlr_device->ctrlr;
	ns_device->ctrlr_device = ctrlr_device;
	ns_device->nsid = nsid;
	rv = snprintf(ns_device->dev_name, sizeof(ns_device->dev_name), "%sn%d",
		      ctrlr_device->dev_name, ns_device->nsid);
	if (rv < 0) {
		SPDK_ERRLOG("Device name too long.\n");
		free(ns_device);
		return -ENAMETOOLONG;
	}

	rv = pthread_create(&ns_device->tid, NULL, cuse_thread, ns_device);
	if (rv != 0) {
		SPDK_ERRLOG("pthread_create failed\n");
		return -rv;
	}

	ns_device->is_started = true;

	return 0;
}

static void
cuse_nvme_ns_stop(struct cuse_device *ctrlr_device, uint32_t nsid)
{
	struct cuse_device *ns_device;

	ns_device = &ctrlr_device->ns_devices[nsid - 1];
	if (!ns_device->is_started) {
		return;
	}

	fuse_session_exit(ns_device->session);
	pthread_join(ns_device->tid, NULL);
	ns_device->is_started = false;
}

static int
nvme_cuse_claim(struct cuse_device *ctrlr_device, uint32_t index)
{
	int dev_fd;
	int pid;
	void *dev_map;
	struct flock cusedev_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	snprintf(ctrlr_device->lock_name, sizeof(ctrlr_device->lock_name),
		 "/var/tmp/spdk_nvme_cuse_lock_%" PRIu32, index);

	dev_fd = open(ctrlr_device->lock_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (dev_fd == -1) {
		SPDK_ERRLOG("could not open %s\n", ctrlr_device->lock_name);
		return -errno;
	}

	if (ftruncate(dev_fd, sizeof(int)) != 0) {
		SPDK_ERRLOG("could not truncate %s\n", ctrlr_device->lock_name);
		close(dev_fd);
		return -errno;
	}

	dev_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
		       MAP_SHARED, dev_fd, 0);
	if (dev_map == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap dev %s (%d)\n", ctrlr_device->lock_name, errno);
		close(dev_fd);
		return -errno;
	}

	if (fcntl(dev_fd, F_SETLK, &cusedev_lock) != 0) {
		pid = *(int *)dev_map;
		SPDK_ERRLOG("Cannot create lock on device %s, probably"
			    " process %d has claimed it\n", ctrlr_device->lock_name, pid);
		munmap(dev_map, sizeof(int));
		close(dev_fd);
		/* F_SETLK returns unspecified errnos, normalize them */
		return -EACCES;
	}

	*(int *)dev_map = (int)getpid();
	munmap(dev_map, sizeof(int));
	ctrlr_device->claim_fd = dev_fd;
	ctrlr_device->index = index;
	/* Keep dev_fd open to maintain the lock. */
	return 0;
}

static void
nvme_cuse_unclaim(struct cuse_device *ctrlr_device)
{
	close(ctrlr_device->claim_fd);
	ctrlr_device->claim_fd = -1;
	unlink(ctrlr_device->lock_name);
}

static void
cuse_nvme_ctrlr_stop(struct cuse_device *ctrlr_device)
{
	uint32_t i;
	uint32_t num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr_device->ctrlr);

	for (i = 1; i <= num_ns; i++) {
		cuse_nvme_ns_stop(ctrlr_device, i);
	}

	fuse_session_exit(ctrlr_device->session);
	pthread_join(ctrlr_device->tid, NULL);
	TAILQ_REMOVE(&g_ctrlr_ctx_head, ctrlr_device, tailq);
	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);
	if (spdk_bit_array_count_set(g_ctrlr_started) == 0) {
		spdk_bit_array_free(&g_ctrlr_started);
	}
	nvme_cuse_unclaim(ctrlr_device);
	free(ctrlr_device->ns_devices);
	free(ctrlr_device);
}

static int
cuse_nvme_ctrlr_update_namespaces(struct cuse_device *ctrlr_device)
{
	uint32_t nsid;
	uint32_t num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr_device->ctrlr);

	for (nsid = 1; nsid <= num_ns; nsid++) {
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr_device->ctrlr, nsid)) {
			cuse_nvme_ns_stop(ctrlr_device, nsid);
			continue;
		}

		if (cuse_nvme_ns_start(ctrlr_device, nsid) < 0) {
			SPDK_ERRLOG("Cannot start CUSE namespace device.");
			return -1;
		}
	}

	return 0;
}

static int
nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	int rv = 0;
	struct cuse_device *ctrlr_device;
	uint32_t num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);

	SPDK_NOTICELOG("Creating cuse device for controller\n");

	if (g_ctrlr_started == NULL) {
		g_ctrlr_started = spdk_bit_array_create(128);
		if (g_ctrlr_started == NULL) {
			SPDK_ERRLOG("Cannot create bit array\n");
			return -ENOMEM;
		}
	}

	ctrlr_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot allocate memory for ctrlr_device.");
		rv = -ENOMEM;
		goto err2;
	}

	ctrlr_device->ctrlr = ctrlr;

	/* Check if device already exists, if not increment index until success */
	ctrlr_device->index = 0;
	while (1) {
		ctrlr_device->index = spdk_bit_array_find_first_clear(g_ctrlr_started, ctrlr_device->index);
		if (ctrlr_device->index == UINT32_MAX) {
			SPDK_ERRLOG("Too many registered controllers\n");
			goto err2;
		}

		if (nvme_cuse_claim(ctrlr_device, ctrlr_device->index) == 0) {
			break;
		}
		ctrlr_device->index++;
	}
	spdk_bit_array_set(g_ctrlr_started, ctrlr_device->index);
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name), "spdk/nvme%d",
		 ctrlr_device->index);

	rv = pthread_create(&ctrlr_device->tid, NULL, cuse_thread, ctrlr_device);
	if (rv != 0) {
		SPDK_ERRLOG("pthread_create failed\n");
		rv = -rv;
		goto err3;
	}
	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);

	ctrlr_device->ns_devices = (struct cuse_device *)calloc(num_ns, sizeof(struct cuse_device));
	/* Start all active namespaces */
	if (cuse_nvme_ctrlr_update_namespaces(ctrlr_device) < 0) {
		SPDK_ERRLOG("Cannot start CUSE namespace devices.");
		cuse_nvme_ctrlr_stop(ctrlr_device);
		rv = -1;
		goto err3;
	}

	return 0;

err3:
	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);
err2:
	free(ctrlr_device);
	if (spdk_bit_array_count_set(g_ctrlr_started) == 0) {
		spdk_bit_array_free(&g_ctrlr_started);
	}
	return rv;
}

static struct cuse_device *
nvme_cuse_get_cuse_ctrlr_device(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device = NULL;

	TAILQ_FOREACH(ctrlr_device, &g_ctrlr_ctx_head, tailq) {
		if (ctrlr_device->ctrlr == ctrlr) {
			break;
		}
	}

	return ctrlr_device;
}

static struct cuse_device *
nvme_cuse_get_cuse_ns_device(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct cuse_device *ctrlr_device = NULL;
	uint32_t num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);

	if (nsid < 1 || nsid > num_ns) {
		return NULL;
	}

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		return NULL;
	}

	if (!ctrlr_device->ns_devices[nsid - 1].is_started) {
		return NULL;
	}

	return &ctrlr_device->ns_devices[nsid - 1];
}

static void
nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		pthread_mutex_unlock(&g_cuse_mtx);
		return;
	}

	cuse_nvme_ctrlr_stop(ctrlr_device);

	pthread_mutex_unlock(&g_cuse_mtx);
}

static void
nvme_cuse_update(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		pthread_mutex_unlock(&g_cuse_mtx);
		return;
	}

	cuse_nvme_ctrlr_update_namespaces(ctrlr_device);

	pthread_mutex_unlock(&g_cuse_mtx);
}

static struct nvme_io_msg_producer cuse_nvme_io_msg_producer = {
	.name = "cuse",
	.stop = nvme_cuse_stop,
	.update = nvme_cuse_update,
};

int
spdk_nvme_cuse_register(struct spdk_nvme_ctrlr *ctrlr)
{
	int rc;

	rc = nvme_io_msg_ctrlr_register(ctrlr, &cuse_nvme_io_msg_producer);
	if (rc) {
		return rc;
	}

	pthread_mutex_lock(&g_cuse_mtx);

	rc = nvme_cuse_start(ctrlr);
	if (rc) {
		nvme_io_msg_ctrlr_unregister(ctrlr, &cuse_nvme_io_msg_producer);
	}

	pthread_mutex_unlock(&g_cuse_mtx);

	return rc;
}

int
spdk_nvme_cuse_unregister(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	cuse_nvme_ctrlr_stop(ctrlr_device);

	pthread_mutex_unlock(&g_cuse_mtx);

	nvme_io_msg_ctrlr_unregister(ctrlr, &cuse_nvme_io_msg_producer);

	return 0;
}

void
spdk_nvme_cuse_update_namespaces(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_cuse_update(ctrlr);
}

int
spdk_nvme_cuse_get_ctrlr_name(struct spdk_nvme_ctrlr *ctrlr, char *name, size_t *size)
{
	struct cuse_device *ctrlr_device;
	size_t req_len;

	pthread_mutex_lock(&g_cuse_mtx);

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	req_len = strnlen(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name));
	if (*size < req_len) {
		*size = req_len;
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENOSPC;
	}
	snprintf(name, req_len + 1, "%s", ctrlr_device->dev_name);

	pthread_mutex_unlock(&g_cuse_mtx);

	return 0;
}

int
spdk_nvme_cuse_get_ns_name(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, char *name, size_t *size)
{
	struct cuse_device *ns_device;
	size_t req_len;

	pthread_mutex_lock(&g_cuse_mtx);

	ns_device = nvme_cuse_get_cuse_ns_device(ctrlr, nsid);
	if (!ns_device) {
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENODEV;
	}

	req_len = strnlen(ns_device->dev_name, sizeof(ns_device->dev_name));
	if (*size < req_len) {
		*size = req_len;
		pthread_mutex_unlock(&g_cuse_mtx);
		return -ENOSPC;
	}
	snprintf(name, req_len + 1, "%s", ns_device->dev_name);

	pthread_mutex_unlock(&g_cuse_mtx);

	return 0;
}
