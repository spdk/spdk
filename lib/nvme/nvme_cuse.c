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
	char				dev_name[128];
	uint32_t			index;
	int				claim_fd;
	char				lock_name[64];

	struct spdk_nvme_ctrlr		*ctrlr;		/**< NVMe controller */
	uint32_t			nsid;		/**< NVMe name space id, or 0 */

	pthread_t			tid;
	struct fuse_session		*session;

	struct cuse_device		*ctrlr_device;
	TAILQ_HEAD(, cuse_device)	ns_devices;

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
	uint16_t			apptag;
	uint16_t			appmask;

	void				*data;
	void				*metadata;

	int				data_len;
	int				metadata_len;

	fuse_req_t			req;
};

static void
cuse_io_ctx_free(struct cuse_io_ctx *ctx)
{
	spdk_free(ctx->data);
	spdk_free(ctx->metadata);
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

#define FUSE_MAX_SIZE 128*1024

static bool
fuse_check_req_size(fuse_req_t req, struct iovec iov[], int iovcnt)
{
	int total_iov_len = 0;
	for (int i = 0; i < iovcnt; i++) {
		total_iov_len += iov[i].iov_len;
		if (total_iov_len > FUSE_MAX_SIZE) {
			fuse_reply_err(req, ENOMEM);
			SPDK_ERRLOG("FUSE request cannot be larger that %d\n", FUSE_MAX_SIZE);
			return false;
		}
	}
	return true;
}

static void
cuse_nvme_passthru_cmd_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_io_ctx *ctx = arg;
	struct iovec out_iov[3];
	struct spdk_nvme_cpl _cpl;
	int out_iovcnt = 0;
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */

	memcpy(&_cpl, cpl, sizeof(struct spdk_nvme_cpl));
	out_iov[out_iovcnt].iov_base = &_cpl.cdw0;
	out_iov[out_iovcnt].iov_len = sizeof(_cpl.cdw0);
	out_iovcnt += 1;

	if (ctx->data_transfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (ctx->data_len > 0) {
			out_iov[out_iovcnt].iov_base = ctx->data;
			out_iov[out_iovcnt].iov_len = ctx->data_len;
			out_iovcnt += 1;
		}
		if (ctx->metadata_len > 0) {
			out_iov[out_iovcnt].iov_base = ctx->metadata;
			out_iov[out_iovcnt].iov_len = ctx->metadata_len;
			out_iovcnt += 1;
		}
	}

	fuse_reply_ioctl_iov(ctx->req, status_field, out_iov, out_iovcnt);
	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_passthru_cmd_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;

	if (nsid != 0) {
		rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, ctrlr->external_io_msgs_qpair, &ctx->nvme_cmd,
							ctx->data,
							ctx->data_len, ctx->metadata, cuse_nvme_passthru_cmd_cb, (void *)ctx);
	} else {
		rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &ctx->nvme_cmd, ctx->data, ctx->data_len,
						   cuse_nvme_passthru_cmd_cb, (void *)ctx);
	}
	if (rc < 0) {
		fuse_reply_err(ctx->req, EINVAL);
		cuse_io_ctx_free(ctx);
	}
}

static void
cuse_nvme_passthru_cmd_send(fuse_req_t req, struct nvme_passthru_cmd *passthru_cmd,
			    const void *data, const void *metadata, int cmd)
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
	ctx->data_transfer = spdk_nvme_opc_get_data_transfer(passthru_cmd->opcode);

	memset(&ctx->nvme_cmd, 0, sizeof(ctx->nvme_cmd));
	ctx->nvme_cmd.opc = passthru_cmd->opcode;
	ctx->nvme_cmd.nsid = passthru_cmd->nsid;
	ctx->nvme_cmd.cdw10 = passthru_cmd->cdw10;
	ctx->nvme_cmd.cdw11 = passthru_cmd->cdw11;
	ctx->nvme_cmd.cdw12 = passthru_cmd->cdw12;
	ctx->nvme_cmd.cdw13 = passthru_cmd->cdw13;
	ctx->nvme_cmd.cdw14 = passthru_cmd->cdw14;
	ctx->nvme_cmd.cdw15 = passthru_cmd->cdw15;

	ctx->data_len = passthru_cmd->data_len;
	ctx->metadata_len = passthru_cmd->metadata_len;

	if (ctx->data_len > 0) {
		ctx->data = spdk_malloc(ctx->data_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
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

	if (ctx->metadata_len > 0) {
		ctx->metadata = spdk_malloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!ctx->metadata) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);
			return;
		}
		if (metadata != NULL) {
			memcpy(ctx->metadata, metadata, ctx->metadata_len);
		}
	}

	if ((unsigned int)cmd != NVME_IOCTL_ADMIN_CMD) {
		/* Send NS for IO IOCTLs */
		rv = nvme_io_msg_send(cuse_device->ctrlr, passthru_cmd->nsid, cuse_nvme_passthru_cmd_execute, ctx);
	} else {
		/* NS == 0 for Admin IOCTLs */
		rv = nvme_io_msg_send(cuse_device->ctrlr, 0, cuse_nvme_passthru_cmd_execute, ctx);
	}
	if (rv) {
		SPDK_ERRLOG("Cannot send io msg to the controller\n");
		fuse_reply_err(req, -rv);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_passthru_cmd(fuse_req_t req, int cmd, void *arg,
		       struct fuse_file_info *fi, unsigned flags,
		       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct nvme_passthru_cmd *passthru_cmd;
	struct iovec in_iov[3], out_iov[3];
	int in_iovcnt = 0, out_iovcnt = 0;
	const void *dptr = NULL, *mdptr = NULL;
	enum spdk_nvme_data_transfer data_transfer;

	in_iov[in_iovcnt].iov_base = (void *)arg;
	in_iov[in_iovcnt].iov_len = sizeof(*passthru_cmd);
	in_iovcnt += 1;
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, out_iovcnt);
		return;
	}

	passthru_cmd = (struct nvme_passthru_cmd *)in_buf;
	data_transfer = spdk_nvme_opc_get_data_transfer(passthru_cmd->opcode);

	if (data_transfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		/* Make data pointer accessible (RO) */
		if (passthru_cmd->addr != 0) {
			in_iov[in_iovcnt].iov_base = (void *)passthru_cmd->addr;
			in_iov[in_iovcnt].iov_len = passthru_cmd->data_len;
			in_iovcnt += 1;
		}
		/* Make metadata pointer accessible (RO) */
		if (passthru_cmd->metadata != 0) {
			in_iov[in_iovcnt].iov_base = (void *)passthru_cmd->metadata;
			in_iov[in_iovcnt].iov_len = passthru_cmd->metadata_len;
			in_iovcnt += 1;
		}
	}

	if (!fuse_check_req_size(req, in_iov, in_iovcnt)) {
		return;
	}
	/* Always make result field writeable regardless of data transfer bits */
	out_iov[out_iovcnt].iov_base = &((struct nvme_passthru_cmd *)arg)->result;
	out_iov[out_iovcnt].iov_len = sizeof(uint32_t);
	out_iovcnt += 1;

	if (data_transfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* Make data pointer accessible (WO) */
		if (passthru_cmd->data_len > 0) {
			out_iov[out_iovcnt].iov_base = (void *)passthru_cmd->addr;
			out_iov[out_iovcnt].iov_len = passthru_cmd->data_len;
			out_iovcnt += 1;
		}
		/* Make metadata pointer accessible (WO) */
		if (passthru_cmd->metadata_len > 0) {
			out_iov[out_iovcnt].iov_base = (void *)passthru_cmd->metadata;
			out_iov[out_iovcnt].iov_len = passthru_cmd->metadata_len;
			out_iovcnt += 1;
		}
	}

	if (!fuse_check_req_size(req, out_iov, out_iovcnt)) {
		return;
	}

	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, out_iov, out_iovcnt);
		return;
	}

	if (data_transfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (data_transfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		dptr = (passthru_cmd->addr == 0) ? NULL : in_buf + sizeof(*passthru_cmd);
		mdptr = (passthru_cmd->metadata == 0) ? NULL : in_buf + sizeof(*passthru_cmd) +
			passthru_cmd->data_len;
	}

	cuse_nvme_passthru_cmd_send(req, passthru_cmd, dptr, mdptr, cmd);
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
cuse_nvme_subsys_reset_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	fuse_req_t req = arg;

	rc = spdk_nvme_ctrlr_reset_subsystem(ctrlr);
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

	if (cmd == NVME_IOCTL_SUBSYS_RESET) {
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_SUBSYS_RESET\n");
		rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_subsys_reset_execute,
				      (void *)req);
	} else {
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_RESET\n");
		rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_reset_execute, (void *)req);
	}
	if (rv) {
		SPDK_ERRLOG("Cannot send reset\n");
		fuse_reply_err(req, EINVAL);
	}
}

static void
cuse_nvme_rescan_execute(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	fuse_req_t req = arg;

	nvme_ctrlr_update_namespaces(ctrlr);
	fuse_reply_ioctl_iov(req, 0, NULL, 0);
}

static void
cuse_nvme_rescan(fuse_req_t req, int cmd, void *arg,
		 struct fuse_file_info *fi, unsigned flags,
		 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int rv;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	if (cuse_device->nsid) {
		SPDK_ERRLOG("Namespace rescan not supported\n");
		fuse_reply_err(req, EINVAL);
		return;
	}

	rv = nvme_io_msg_send(cuse_device->ctrlr, cuse_device->nsid, cuse_nvme_rescan_execute, (void *)req);
	if (rv) {
		SPDK_ERRLOG("Cannot send rescan\n");
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
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */

	fuse_reply_ioctl_iov(ctx->req, status_field, NULL, 0);

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_submit_io_write_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	rc = spdk_nvme_ns_cmd_write_with_md(ns, ctrlr->external_io_msgs_qpair, ctx->data, ctx->metadata,
					    ctx->lba, /* LBA start */
					    ctx->lba_count, /* number of LBAs */
					    cuse_nvme_submit_io_write_done, ctx, 0,
					    ctx->appmask, ctx->apptag);

	if (rc != 0) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_submit_io_write(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags, uint32_t block_size, uint32_t md_size,
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

	if (user_io->metadata) {
		ctx->apptag = user_io->apptag;
		ctx->appmask = user_io->appmask;
		ctx->metadata_len = md_size * ctx->lba_count;
		ctx->metadata = spdk_zmalloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		if (ctx->metadata == NULL) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			if (ctx->metadata_len == 0) {
				SPDK_ERRLOG("Device format does not support metadata\n");
			}
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);
			return;
		}

		memcpy(ctx->metadata, in_buf + sizeof(*user_io) + ctx->data_len, ctx->metadata_len);
	}

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
	struct iovec iov[2];
	int iovcnt = 0;
	uint16_t status_field = cpl->status_raw >> 1; /* Drop out phase bit */

	iov[iovcnt].iov_base = ctx->data;
	iov[iovcnt].iov_len = ctx->data_len;
	iovcnt += 1;

	if (ctx->metadata) {
		iov[iovcnt].iov_base = ctx->metadata;
		iov[iovcnt].iov_len = ctx->metadata_len;
		iovcnt += 1;
	}

	fuse_reply_ioctl_iov(ctx->req, status_field, iov, iovcnt);

	cuse_io_ctx_free(ctx);
}

static void
cuse_nvme_submit_io_read_cb(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, void *arg)
{
	int rc;
	struct cuse_io_ctx *ctx = arg;
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

	rc = spdk_nvme_ns_cmd_read_with_md(ns, ctrlr->external_io_msgs_qpair, ctx->data, ctx->metadata,
					   ctx->lba, /* LBA start */
					   ctx->lba_count, /* number of LBAs */
					   cuse_nvme_submit_io_read_done, ctx, 0,
					   ctx->appmask, ctx->apptag);

	if (rc != 0) {
		SPDK_ERRLOG("read failed: rc = %d\n", rc);
		fuse_reply_err(ctx->req, rc);
		cuse_io_ctx_free(ctx);
		return;
	}
}

static void
cuse_nvme_submit_io_read(struct cuse_device *cuse_device, fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned flags, uint32_t block_size, uint32_t md_size,
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

	if (user_io->metadata) {
		ctx->apptag = user_io->apptag;
		ctx->appmask = user_io->appmask;
		ctx->metadata_len = md_size * ctx->lba_count;
		ctx->metadata = spdk_zmalloc(ctx->metadata_len, 4096, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

		if (ctx->metadata == NULL) {
			SPDK_ERRLOG("Cannot allocate memory for metadata\n");
			if (ctx->metadata_len == 0) {
				SPDK_ERRLOG("Device format does not support metadata\n");
			}
			fuse_reply_err(req, ENOMEM);
			cuse_io_ctx_free(ctx);
			return;
		}
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
	struct iovec in_iov[3], out_iov[2];
	int in_iovcnt = 0, out_iovcnt = 0;
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	struct spdk_nvme_ns *ns;
	uint32_t block_size;
	uint32_t md_size;

	in_iov[in_iovcnt].iov_base = (void *)arg;
	in_iov[in_iovcnt].iov_len = sizeof(*user_io);
	in_iovcnt += 1;
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, 0);
		return;
	}

	user_io = in_buf;

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	block_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	switch (user_io->opcode) {
	case SPDK_NVME_OPC_READ:
		out_iov[out_iovcnt].iov_base = (void *)user_io->addr;
		out_iov[out_iovcnt].iov_len = (user_io->nblocks + 1) * block_size;
		out_iovcnt += 1;
		if (user_io->metadata != 0) {
			out_iov[out_iovcnt].iov_base = (void *)user_io->metadata;
			out_iov[out_iovcnt].iov_len = (user_io->nblocks + 1) * md_size;
			out_iovcnt += 1;
		}
		if (!fuse_check_req_size(req, out_iov, out_iovcnt)) {
			return;
		}
		if (out_bufsz == 0) {
			fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, out_iov, out_iovcnt);
			return;
		}

		cuse_nvme_submit_io_read(cuse_device, req, cmd, arg, fi, flags,
					 block_size, md_size, in_buf, in_bufsz, out_bufsz);
		break;
	case SPDK_NVME_OPC_WRITE:
		in_iov[in_iovcnt].iov_base = (void *)user_io->addr;
		in_iov[in_iovcnt].iov_len = (user_io->nblocks + 1) * block_size;
		in_iovcnt += 1;
		if (user_io->metadata != 0) {
			in_iov[in_iovcnt].iov_base = (void *)user_io->metadata;
			in_iov[in_iovcnt].iov_len = (user_io->nblocks + 1) * md_size;
			in_iovcnt += 1;
		}
		if (!fuse_check_req_size(req, in_iov, in_iovcnt)) {
			return;
		}
		if (in_bufsz == sizeof(*user_io)) {
			fuse_reply_ioctl_retry(req, in_iov, in_iovcnt, NULL, out_iovcnt);
			return;
		}

		cuse_nvme_submit_io_write(cuse_device, req, cmd, arg, fi, flags,
					  block_size, md_size, in_buf, in_bufsz, out_bufsz);
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
cuse_blkgetsectorsize(fuse_req_t req, int cmd, void *arg,
		      struct fuse_file_info *fi, unsigned flags,
		      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int ssize;
	struct spdk_nvme_ns *ns;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	FUSE_REPLY_CHECK_BUFFER(req, arg, out_bufsz, ssize);

	ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	ssize = spdk_nvme_ns_get_sector_size(ns);
	fuse_reply_ioctl(req, 0, &ssize, sizeof(ssize));
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
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ADMIN_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESET:
	case NVME_IOCTL_SUBSYS_RESET:
		cuse_nvme_reset(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_RESCAN:
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_RESCAN\n");
		cuse_nvme_rescan(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, ENOTTY);
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
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ADMIN_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_SUBMIT_IO:
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_SUBMIT_IO\n");
		cuse_nvme_submit_io(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_IO_CMD:
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_IO_CMD\n");
		cuse_nvme_passthru_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		SPDK_DEBUGLOG(nvme_cuse, "NVME_IOCTL_ID\n");
		cuse_getid(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKPBSZGET:
		SPDK_DEBUGLOG(nvme_cuse, "BLKPBSZGET\n");
		cuse_blkpbszget(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKSSZGET:
		SPDK_DEBUGLOG(nvme_cuse, "BLKSSZGET\n");
		cuse_blkgetsectorsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE:
		SPDK_DEBUGLOG(nvme_cuse, "BLKGETSIZE\n");
		/* Returns the device size as a number of 512-byte blocks (returns pointer to long) */
		cuse_blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE64:
		SPDK_DEBUGLOG(nvme_cuse, "BLKGETSIZE64\n");
		/* Returns the device size in sectors (returns pointer to uint64_t) */
		cuse_blkgetsize64(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, ENOTTY);
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

static int cuse_session_create(struct cuse_device *cuse_device)
{
	char *cuse_argv[] = { "cuse", "-f" };
	int multithreaded;
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	struct cuse_info ci;
	char devname_arg[128 + 8];
	const char *dev_info_argv[] = { devname_arg };

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
		return -1;
	}
	SPDK_NOTICELOG("fuse session for device %s created\n", cuse_device->dev_name);
	return 0;
}

static void *
cuse_thread(void *arg)
{
	struct cuse_device *cuse_device = arg;
	int rc;
	struct fuse_buf buf = { .mem = NULL };
	struct pollfd fds;
	int timeout_msecs = 500;

	spdk_unaffinitize_thread();

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
	pthread_exit(NULL);
}

static struct cuse_device *nvme_cuse_get_cuse_ns_device(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t nsid);

/*****************************************************************************
 * CUSE devices management
 */

static int
cuse_nvme_ns_start(struct cuse_device *ctrlr_device, uint32_t nsid)
{
	struct cuse_device *ns_device;
	int rv;

	ns_device = nvme_cuse_get_cuse_ns_device(ctrlr_device->ctrlr, nsid);
	if (ns_device != NULL) {
		return 0;
	}

	ns_device = calloc(1, sizeof(struct cuse_device));
	if (ns_device == NULL) {
		return -ENOMEM;
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
	rv = cuse_session_create(ns_device);
	if (rv != 0) {
		free(ns_device);
		return rv;
	}
	rv = pthread_create(&ns_device->tid, NULL, cuse_thread, ns_device);
	if (rv != 0) {
		SPDK_ERRLOG("pthread_create failed\n");
		free(ns_device);
		return -rv;
	}
	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_device, tailq);

	return 0;
}

static void
cuse_nvme_ns_stop(struct cuse_device *ctrlr_device, struct cuse_device *ns_device)
{
	if (ns_device->session != NULL) {
		fuse_session_exit(ns_device->session);
	}
	pthread_join(ns_device->tid, NULL);
	TAILQ_REMOVE(&ctrlr_device->ns_devices, ns_device, tailq);
	if (ns_device->session != NULL) {
		cuse_lowlevel_teardown(ns_device->session);
	}
	free(ns_device);
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
	struct cuse_device *ns_device, *tmp;

	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		cuse_nvme_ns_stop(ctrlr_device, ns_device);
	}

	assert(TAILQ_EMPTY(&ctrlr_device->ns_devices));

	fuse_session_exit(ctrlr_device->session);
	pthread_join(ctrlr_device->tid, NULL);
	TAILQ_REMOVE(&g_ctrlr_ctx_head, ctrlr_device, tailq);
	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);
	if (spdk_bit_array_count_set(g_ctrlr_started) == 0) {
		spdk_bit_array_free(&g_ctrlr_started);
	}
	nvme_cuse_unclaim(ctrlr_device);
	if (ctrlr_device->session != NULL) {
		cuse_lowlevel_teardown(ctrlr_device->session);
	}
	free(ctrlr_device);
}

static int
cuse_nvme_ctrlr_update_namespaces(struct cuse_device *ctrlr_device)
{
	struct cuse_device *ns_device, *tmp;
	uint32_t nsid;

	/* Remove namespaces that have disappeared */
	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr_device->ctrlr, ns_device->nsid)) {
			cuse_nvme_ns_stop(ctrlr_device, ns_device);
		}
	}

	/* Add new namespaces */
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr_device->ctrlr);
	while (nsid != 0) {
		if (cuse_nvme_ns_start(ctrlr_device, nsid) < 0) {
			SPDK_ERRLOG("Cannot start CUSE namespace device.");
			return -1;
		}

		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr_device->ctrlr, nsid);
	}

	return 0;
}

static int
nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	int rv = 0;
	struct cuse_device *ctrlr_device;

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
		goto free_device;
	}

	ctrlr_device->ctrlr = ctrlr;

	/* Check if device already exists, if not increment index until success */
	ctrlr_device->index = 0;
	while (1) {
		ctrlr_device->index = spdk_bit_array_find_first_clear(g_ctrlr_started, ctrlr_device->index);
		if (ctrlr_device->index == UINT32_MAX) {
			SPDK_ERRLOG("Too many registered controllers\n");
			goto free_device;
		}

		if (nvme_cuse_claim(ctrlr_device, ctrlr_device->index) == 0) {
			break;
		}
		ctrlr_device->index++;
	}
	spdk_bit_array_set(g_ctrlr_started, ctrlr_device->index);
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name), "spdk/nvme%d",
		 ctrlr_device->index);

	rv = cuse_session_create(ctrlr_device);
	if (rv != 0) {
		goto clear_and_free;
	}

	rv = pthread_create(&ctrlr_device->tid, NULL, cuse_thread, ctrlr_device);
	if (rv != 0) {
		SPDK_ERRLOG("pthread_create failed\n");
		rv = -rv;
		goto clear_and_free;
	}

	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);

	TAILQ_INIT(&ctrlr_device->ns_devices);

	/* Start all active namespaces */
	if (cuse_nvme_ctrlr_update_namespaces(ctrlr_device) < 0) {
		SPDK_ERRLOG("Cannot start CUSE namespace devices.");
		cuse_nvme_ctrlr_stop(ctrlr_device);
		rv = -1;
		goto clear_and_free;
	}

	return 0;

clear_and_free:
	spdk_bit_array_clear(g_ctrlr_started, ctrlr_device->index);
free_device:
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
	struct cuse_device *ns_device;

	ctrlr_device = nvme_cuse_get_cuse_ctrlr_device(ctrlr);
	if (!ctrlr_device) {
		return NULL;
	}

	TAILQ_FOREACH(ns_device, &ctrlr_device->ns_devices, tailq) {
		if (ns_device->nsid == nsid) {
			return ns_device;
		}
	}

	return NULL;
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

SPDK_LOG_REGISTER_COMPONENT(nvme_cuse)
