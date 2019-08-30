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

static int g_controllers_found = 0;

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

struct cuse_nvme_cpl {
	bool			done;
	struct spdk_nvme_cpl	cpl;
};

static void
cuse_cmd_done(void *_done, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_nvme_cpl *cuse_cpl = _done;

	cuse_cpl->cpl = *cpl;
	cuse_cpl->done = true;
}

struct cuse_ctx {
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			idx;
	struct spdk_nvme_ns		*ns;
	uint32_t			nsid;
	pthread_t			tid;
	struct fuse_session		*session;
	TAILQ_ENTRY(cuse_ctx)		tailq;
};

static void nvme_admin_cmd(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct cuse_ctx *ctrlr_ctx = fuse_req_userdata(req);
	struct nvme_admin_cmd *admin_cmd;
	struct iovec in_iov, out_iov[2];
	struct spdk_nvme_cmd nvme_cmd;
	void *buf = NULL;
	struct cuse_nvme_cpl cuse_cpl;

	in_iov.iov_base = arg;
	in_iov.iov_len = sizeof(*admin_cmd);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
		return;
	}

	admin_cmd = (struct nvme_admin_cmd *)in_buf;

	switch (spdk_nvme_opc_get_data_transfer(admin_cmd->opcode)) {
	case SPDK_NVME_DATA_NONE:
		printf("SPDK_NVME_DATA_NONE\n");
		fuse_reply_err(req, EINVAL);
		break;
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		printf("SPDK_NVME_DATA_HOST_TO_CONTROLLER\n");
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

		memset(&nvme_cmd, 0, sizeof(nvme_cmd));
		nvme_cmd.opc = admin_cmd->opcode;
		nvme_cmd.nsid = admin_cmd->nsid;
		nvme_cmd.cdw10 = admin_cmd->cdw10;
		nvme_cmd.cdw11 = admin_cmd->cdw11;
		nvme_cmd.cdw12 = admin_cmd->cdw12;
		nvme_cmd.cdw13 = admin_cmd->cdw13;
		nvme_cmd.cdw14 = admin_cmd->cdw14;
		nvme_cmd.cdw15 = admin_cmd->cdw15;
		cuse_cpl.done = false;
		if (admin_cmd->data_len > 0) {
			buf = spdk_malloc(admin_cmd->data_len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		}
		spdk_nvme_ctrlr_cmd_admin_raw(ctrlr_ctx->ctrlr, &nvme_cmd, buf, admin_cmd->data_len,
					      cuse_cmd_done, (void *)&cuse_cpl);
		while (cuse_cpl.done == false) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr_ctx->ctrlr);
		}
		out_iov[0].iov_base = &cuse_cpl.cpl.cdw0;
		out_iov[0].iov_len = sizeof(cuse_cpl.cpl.cdw0);
		if (admin_cmd->data_len > 0) {
			out_iov[1].iov_base = buf;
			out_iov[1].iov_len = admin_cmd->data_len;
			fuse_reply_ioctl_iov(req, 0, out_iov, 2);
			spdk_free(buf);
		} else {
			fuse_reply_ioctl_iov(req, 0, out_iov, 1);
		}
		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		fuse_reply_err(req, EINVAL);
		break;
	}
}

uint8_t __data[512];

static void nvme_submit_io(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_user_io *user_io = in_buf;
	struct iovec in_iov[2], out_iov, iov;

	in_iov[0].iov_base = arg;
	in_iov[0].iov_len = sizeof(*user_io);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);
		return;
	}

	switch (user_io->opcode) {
	case SPDK_NVME_OPC_READ:
		out_iov.iov_base = (void *)user_io->addr;
		out_iov.iov_len = (user_io->nblocks + 1) * 512;
		if (out_bufsz == 0) {
			fuse_reply_ioctl_retry(req, in_iov, 1, &out_iov, 1);
			return;
		}

		iov.iov_base = __data;
		iov.iov_len = sizeof(__data);
		fuse_reply_ioctl_iov(req, 0, &iov, 1);
		break;
	case SPDK_NVME_OPC_WRITE:
		in_iov[1].iov_base = (void *)user_io->addr;
		in_iov[1].iov_len = (user_io->nblocks + 1) * 512;
		if (in_bufsz == sizeof(*user_io)) {
			fuse_reply_ioctl_retry(req, in_iov, 2, NULL, 0);
			return;
		}

		memcpy(__data, in_buf + sizeof(*user_io), 512);
		fuse_reply_ioctl_iov(req, 0, NULL, 0);
		break;
	case SPDK_NVME_OPC_COMPARE:
		fprintf(stderr, "SUBMIT_IO: SPDK_NVME_OPC_COMPARE not implemented yet\n");
		fuse_reply_err(req, EINVAL);
	default:
		fprintf(stderr, "SUBMIT_IO: opc:%d not valid\n", user_io->opcode);
		fuse_reply_err(req, EINVAL);
		return;
	}

}

static void blkpbszget(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	int pbsz;
	struct iovec out_iov;

	printf("addr=0x%jx\n", (uint64_t)arg);
	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(pbsz);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct cuse_ctx *ctrlr_ctx = fuse_req_userdata(req);
	pbsz = spdk_nvme_ns_get_sector_size(ctrlr_ctx->ns);
	fuse_reply_ioctl(req, 0, &pbsz, sizeof(pbsz));
}

static void blkgetsize(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	long size;
	struct iovec out_iov;

	printf("addr=0x%jx\n", (uint64_t)arg);
	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(size);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct cuse_ctx *ctrlr_ctx = fuse_req_userdata(req);

	size = spdk_nvme_ns_get_num_sectors(ctrlr_ctx->ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void cuse_ctrlr_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		printf(">>> CMD 0x%X -- ENOSYS\n", cmd);
		fuse_reply_err(req, ENOSYS);
		return;
	}

	printf(">>> CMD 0x%X\n", cmd);
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		printf("NVME_IOCTL_ADMIN_CMD\n");
		nvme_admin_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_IO_CMD:
		printf("NVME_IOCTL_IO_CMD\n");
		fuse_reply_err(req, EINVAL);
		break;

	default:
		printf("cmd=0x%x\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

static void cuse_ns_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		printf(">>> CMD 0x%X -- ENOSYS\n", cmd);
		fuse_reply_err(req, ENOSYS);
		return;
	}

	printf(">>> CMD 0x%X\n", cmd);
	switch (cmd) {
	case NVME_IOCTL_IO_CMD:
		printf("NVME_IOCTL_IO_CMD\n");
		fuse_reply_err(req, EINVAL);
		break;

	case NVME_IOCTL_SUBMIT_IO:
		printf("NVME_IOCTL_SUBMIT_IO\n");
		nvme_submit_io(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_ID:
		printf("NVME_IOCTL_ID\n");
		fuse_reply_err(req, EINVAL);
		break;

	case BLKPBSZGET:
		printf("BLKPBSZGET\n");
		blkpbszget(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKSSZGET:
		/* Logical sector size (long) */
		printf("BLKSSZGET\n");
		fuse_reply_err(req, EINVAL);
		break;

	case BLKGETSIZE:
		/* Returns the device size in sectors (returns pointer to long) */
		printf("BLKGETSIZE\n");
		blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		if ((cmd & 0xFFFFFF00) == 0x00001200) {
			printf("BLK IOCTL %2X\n", cmd & 0xFF);
			switch (cmd) {
			  case BLKROSET:
			  case BLKROGET:
			  case BLKRRPART:
			  case BLKGETSIZE:
			  case BLKFLSBUF:
			  case BLKRASET:
			  case BLKRAGET:
			    break;
			}
			fuse_reply_err(req, EINVAL);
		} else {
			printf("cmd=0x%x\n", cmd);
			fuse_reply_err(req, EINVAL);
		}
	}
}

static const struct cuse_lowlevel_ops cuse_ctrlr_clop = {
	.open		= cuse_open,
	.ioctl		= cuse_ctrlr_ioctl,
};

static void
cuse_ns_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi) {
	printf("... cuse_ns_read ...\n");
	//fuse_reply_buf(req, cusexmp_buf + off, size);
	fuse_reply_err(req, EINVAL);
}

static void
cuse_ns_write(fuse_req_t req, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
	printf("... cuse_ns_write ...\n");

	//fuse_reply_write(req, size);
	fuse_reply_err(req, EINVAL);
}

static void
cuse_ns_flush(fuse_req_t req, struct fuse_file_info *fi) {
	printf("... cuse_ns_flush ...\n");
	fuse_reply_err(req, EINVAL);
}

static const struct cuse_lowlevel_ops cuse_ns_clop = {
	.open		= cuse_open,
	.read		= cuse_ns_read,
	.write		= cuse_ns_write,
	.flush		= cuse_ns_flush,
	.ioctl		= cuse_ns_ioctl,
};

static TAILQ_HEAD(, cuse_ctx) g_ctrlr_ctx_head = TAILQ_HEAD_INITIALIZER(g_ctrlr_ctx_head);
static TAILQ_HEAD(, cuse_ctx) g_ns_ctx_head = TAILQ_HEAD_INITIALIZER(g_ns_ctx_head);

static void *
cuse_ctrlr_thread(void *arg)
{
	struct cuse_ctx *ctrlr_ctx = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	char dev_name[128];
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;
	int multithreaded;

	snprintf(dev_name, sizeof(dev_name), "DEVNAME=nvme%d\n", ctrlr_ctx->idx);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	ctrlr_ctx->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ctrlr_clop, &multithreaded, ctrlr_ctx);
	fuse_session_loop(ctrlr_ctx->session);

	pthread_exit(NULL);
}

static void *
cuse_ns_thread(void *arg)
{
	struct cuse_ctx *ns_ctx = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	char dev_name[128];
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;
	int multithreaded;

	snprintf(dev_name, sizeof(dev_name), "DEVNAME=nvme%dn%d\n", ns_ctx->idx, ns_ctx->nsid);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	ns_ctx->session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ns_clop, &multithreaded, ns_ctx);
	fuse_session_loop(ns_ctx->session);

	pthread_exit(NULL);
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct cuse_ctx *ctrlr_ctx;
	struct cuse_ctx *ns_ctx;
	uint32_t i, nsid;

	ctrlr_ctx = calloc(1, sizeof(*ctrlr_ctx));
	if (ctrlr_ctx == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return;
	}

	ctrlr_ctx->ctrlr = ctrlr;
	ctrlr_ctx->idx = g_controllers_found++;

	if (pthread_create(&ctrlr_ctx->tid, NULL, cuse_ctrlr_thread, ctrlr_ctx)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}

	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_ctx, tailq);

	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		nsid = i + 1;
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		ns_ctx = calloc(1, sizeof(*ns_ctx));
		if (ns_ctx == NULL) {
			SPDK_ERRLOG("calloc failed\n");
			return;
		}

		ns_ctx->ctrlr = ctrlr;
		ns_ctx->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		ns_ctx->nsid = nsid;

		if (pthread_create(&ns_ctx->tid, NULL, cuse_ns_thread, ns_ctx)) {
			SPDK_ERRLOG("pthread_create failed\n");
		}

		TAILQ_INSERT_TAIL(&g_ns_ctx_head, ns_ctx, tailq);
	}
}

static bool g_shutdown = false;

static void
__shutdown_signal(int signo)
{
	g_shutdown = true;
}

int
nvme_cuse_start(void)
{
	int				rc;
	struct cuse_ctx			*ctx, *tmp;

	struct sigaction sigact;
	sigset_t sigmask;

#if 0
	/* Set signal mask for main thread which will then get inherited
	 *  by all of the pthreads spawned for CUSE sessions in the attach
	 *  callback.  Mask SIGINT and SIGTERM but unmask SIGHUP.  SIGHUP
	 *  is what we will use to interrupt the cuse loop to get the
	 *  pthreads to exit.
	 */
	pthread_sigmask(SIG_SETMASK, NULL, &sigmask);
	sigdelset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	pthread_sigmask(SIG_SETMASK, &sigmask, NULL);
#endif

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	/* Now unmask SIGINT and SIGTERM for the main thread so that the
	 *  shutdown signal doesn't get sent to one of the pthreads.
	 */
	sigaddset(&sigmask, SIGHUP);
	sigdelset(&sigmask, SIGINT);
	sigdelset(&sigmask, SIGTERM);
	pthread_sigmask(SIG_SETMASK, &sigmask, NULL);

	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigact.sa_handler = __shutdown_signal;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGHUP, &sigact, NULL);

	if (g_controllers_found == 0) {
		fprintf(stderr, "No NVMe controllers found.\n");
	}

	while (!g_shutdown) {}

	TAILQ_FOREACH_SAFE(ctx, &g_ctrlr_ctx_head, tailq, tmp) {
		fuse_session_exit(ctx->session);
		pthread_kill(ctx->tid, SIGHUP);
		pthread_join(ctx->tid, NULL);
		TAILQ_REMOVE(&g_ctrlr_ctx_head, ctx, tailq);
	}

	TAILQ_FOREACH_SAFE(ctx, &g_ns_ctx_head, tailq, tmp) {
		fuse_session_exit(ctx->session);
		pthread_kill(ctx->tid, SIGHUP);
		pthread_join(ctx->tid, NULL);
		TAILQ_REMOVE(&g_ns_ctx_head, ctx, tailq);
	}

	return 0;
}
