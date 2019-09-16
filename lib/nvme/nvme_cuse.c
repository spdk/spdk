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
	bool is_completed;
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
_cuse_nvme_io_msg_free(struct spdk_nvme_io_msg *io)
{
	if (io->io_channel) {
		spdk_put_io_channel(io->io_channel);
	}
	spdk_free(io->data);
	free(io);
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
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)malloc(sizeof(struct spdk_nvme_io_msg));

	io->ctrlr = cuse_device->ctrlr;

	in_iov.iov_base = arg;
	in_iov.iov_len = sizeof(*admin_cmd);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
		return;
	}

	admin_cmd = (struct nvme_admin_cmd *)in_buf;

	switch (spdk_nvme_opc_get_data_transfer(admin_cmd->opcode)) {
	case SPDK_NVME_DATA_NONE:
		SPDK_ERRLOG("SPDK_NVME_DATA_NONE not implemented\n");
		fuse_reply_err(req, EINVAL);
		break;
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		SPDK_ERRLOG("SPDK_NVME_DATA_HOST_TO_CONTROLLER not implemented\n");
		fuse_reply_err(req, EINVAL);
		break;
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

/*****************************************************************************
 * Namespace IO requests
 */

static void
cuse_nvme_submit_io_write_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)ref;

	fuse_reply_ioctl_iov(io->req, 0, NULL, 0);

	_cuse_nvme_io_msg_free(io);
}

static void
cuse_nvme_submit_io_write_cb(struct spdk_nvme_io_msg *io)
{
	int rc;

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(io->ctrlr, io->nsid);
	uint32_t block_size = spdk_nvme_ns_get_sector_size(ns);

	io->data_len = io->lba_count * block_size;

	printf("block_size: %d\n", block_size);
	printf("data_len: %d\n", io->data_len);

	rc = spdk_nvme_ns_cmd_write(ns, io->qpair, io->data,
				    io->lba, /* LBA start */
				    io->lba_count, /* number of LBAs */
				    cuse_nvme_submit_io_write_done, io, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		fuse_reply_err(io->req, EINVAL);
		_cuse_nvme_io_msg_free(io);
		return;
	}
}

static void
cuse_nvme_submit_io_write(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)calloc(1, sizeof(struct spdk_nvme_io_msg));
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	const struct nvme_user_io *user_io = in_buf;
	struct spdk_nvme_ns *ns;
	uint32_t block_size;

	io->req = req;
	io->nsid = cuse_device->nsid;
	io->ctrlr = cuse_device->ctrlr_device->ctrlr;

	ns = spdk_nvme_ctrlr_get_ns(io->ctrlr, io->nsid);
	block_size = spdk_nvme_ns_get_sector_size(ns);

	/* fill io request with parameters */
	io->lba = user_io->slba;
	io->lba_count = user_io->nblocks + 1;

	io->data_len = io->lba_count * block_size;
	io->data = spdk_nvme_ctrlr_alloc_cmb_io_buffer(io->ctrlr, io->data_len);
	if (io->data == NULL) {
		io->data = spdk_zmalloc(io->data_len, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
					SPDK_MALLOC_DMA);
	}
	if (io->data == NULL) {
		SPDK_ERRLOG("Write buffer allocation failed\n");
		fuse_reply_err(io->req, ENOMEM);
		_cuse_nvme_io_msg_free(io);
		return;
	}

	memcpy(io->data, in_buf + sizeof(*user_io), io->data_len);

	spdk_nvme_io_msg_send(io, cuse_nvme_submit_io_write_cb, NULL);
}

static void
cuse_nvme_submit_io_read_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)ref;
	struct iovec iov;

	iov.iov_base = io->data;
	iov.iov_len = io->data_len;

	fuse_reply_ioctl_iov(io->req, 0, &iov, 1);

	_cuse_nvme_io_msg_free(io);
}

static void
cuse_nvme_submit_io_read_cb(struct spdk_nvme_io_msg *io)
{
	int rc;

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(io->ctrlr, io->nsid);

	uint32_t block_size = spdk_nvme_ns_get_sector_size(ns);

	io->data_len = io->lba_count * block_size;
	io->data = spdk_nvme_ctrlr_alloc_cmb_io_buffer(io->ctrlr, io->data_len);
	if (io->data == NULL) {
		io->data = spdk_zmalloc(io->data_len, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
					SPDK_MALLOC_DMA);
	}
	if (io->data == NULL) {
		SPDK_ERRLOG("Read buffer allocation failed\n");
		fuse_reply_err(io->req, ENOMEM);
		_cuse_nvme_io_msg_free(io);
		return;
	}

	printf("block_size: %d\n", block_size);
	printf("data_len: %d\n", io->data_len);

	rc = spdk_nvme_ns_cmd_read(ns, io->qpair, io->data,
				   io->lba, /* LBA start */
				   io->lba_count, /* number of LBAs */
				   cuse_nvme_submit_io_read_done, io, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("read failed: rc = %d\n", rc);
		fuse_reply_err(io->req, EINVAL);
		_cuse_nvme_io_msg_free(io);
		return;
	}
}

static void
cuse_nvme_submit_io_read(fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned flags,
			 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct spdk_nvme_io_msg *io = (struct spdk_nvme_io_msg *)calloc(1, sizeof(struct spdk_nvme_io_msg));
	struct cuse_device *cuse_device = fuse_req_userdata(req);
	const struct nvme_user_io *user_io = in_buf;

	io->req = req;
	io->nsid = cuse_device->nsid;
	io->ctrlr = cuse_device->ctrlr_device->ctrlr;

	/* fill io request with parameters */
	io->lba = user_io->slba;
	io->lba_count = user_io->nblocks + 1;

	spdk_nvme_io_msg_send(io, cuse_nvme_submit_io_read_cb, NULL);
}


static void
nvme_submit_io(fuse_req_t req, int cmd, void *arg,
	       struct fuse_file_info *fi, unsigned flags,
	       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io = in_buf;
	struct iovec in_iov[2];

	in_iov[0].iov_base = arg;
	in_iov[0].iov_len = sizeof(*user_io);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);
		return;
	}

	switch (user_io->opcode) {
	case SPDK_NVME_OPC_READ:
		cuse_nvme_submit_io_read(req, cmd, arg, fi, flags, in_buf,
					 in_bufsz, out_bufsz);
		break;
	case SPDK_NVME_OPC_WRITE:
		in_iov[1].iov_base = (void *)user_io->addr;
		in_iov[1].iov_len = (user_io->nblocks + 1) * 512;
		if (in_bufsz == sizeof(*user_io)) {
			fuse_reply_ioctl_retry(req, in_iov, 2, NULL, 0);
			return;
		}

		cuse_nvme_submit_io_write(req, cmd, arg, fi, flags, in_buf,
					  in_bufsz, out_bufsz);

		break;
	case SPDK_NVME_OPC_COMPARE:
		SPDK_ERRLOG("SUBMIT_IO: SPDK_NVME_OPC_COMPARE not implemented yet\n");
		fuse_reply_err(req, EINVAL);
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
blkgetsize64(fuse_req_t req, int cmd, void *arg,
	     struct fuse_file_info *fi, unsigned flags,
	     const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	uint64_t size;
	struct iovec out_iov;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(size);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);

	size = spdk_nvme_ns_get_num_sectors(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void
blkpbszget(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int pbsz;
	struct iovec out_iov;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(pbsz);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);
	pbsz = spdk_nvme_ns_get_sector_size(ns);
	fuse_reply_ioctl(req, 0, &pbsz, sizeof(pbsz));
}

static void
blkgetsize(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	long size;
	struct iovec out_iov;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(size);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(cuse_device->ctrlr, cuse_device->nsid);

	/* FIXIT: return size in 512 bytes blocks, not sectors! */
	size = spdk_nvme_ns_get_num_sectors(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void
getid(fuse_req_t req, int cmd, void *arg,
      struct fuse_file_info *fi, unsigned flags,
      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	uint32_t nsid;
	struct iovec out_iov;
	struct cuse_device *cuse_device = fuse_req_userdata(req);

	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(nsid);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	nsid = cuse_device->nsid;

	fuse_reply_ioctl(req, nsid, NULL, 0);
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

	case NVME_IOCTL_RESET: {
		struct cuse_device *ctrlr_device = fuse_req_userdata(req);

		/* FIXIT! This function should be called from a single thread while no
		   other threads are actively using the NVMe device. */
		spdk_nvme_ctrlr_reset(ctrlr_device->ctrlr);
		fuse_reply_err(req, EINVAL);
	}
	break;

	case NVME_IOCTL_IO_CMD:
		fuse_reply_err(req, EINVAL);
		break;

	case NVME_IOCTL_SUBMIT_IO:
		nvme_submit_io(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		getid(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKPBSZGET:
		blkpbszget(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKSSZGET:
		/* Logical sector size (long) */
		SPDK_ERRLOG("BLKSSZGET not implemented yet.\n");
		fuse_reply_err(req, EINVAL);
		break;

	case BLKGETSIZE:
		/* Returns the device size as a number of 512-byte blocks (returns pointer to long) */
		blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE64:
		/* Returns the device size in sectors (returns pointer to uint64_t) */
		blkgetsize64(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

/**
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
 *
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
#if 0
	struct cuse_device *ctrlr_device;
	struct cuse_device *ns_device, *tmp;

	TAILQ_FOREACH_SAFE(ns_device, &ctrlr_device->ns_devices, tailq, tmp) {
		fuse_session_exit(ns_device->session);
		pthread_kill(ns_device->tid, SIGHUP);
		pthread_join(ns_device->tid, NULL);
		TAILQ_REMOVE(&ctrlr_device->ns_devices, ns_device, tailq);
	}

	/* TODO deal with closing ctrlr device */
#endif
	return 0;
}
