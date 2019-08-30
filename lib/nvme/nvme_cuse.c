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

static int g_controllers_found = 0;

static void cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

struct cuse_nvme_io_request {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;

	uint64_t lba;
	uint32_t lba_count;

	fuse_req_t req;

	size_t size;
	off_t off;
	void *data;

	pthread_cond_t cv;
	pthread_mutex_t mp;

	bool is_completed;

	void *buf;
	struct spdk_nvme_qpair *qpair;
};

struct cuse_nvme_cpl {
	bool			done;
	struct spdk_nvme_cpl	cpl;
	fuse_req_t		req;
	void			*buf;
	uint32_t		data_len;
};

/**
 * TODO: cuse_cmd_done should be checked for the case when admin queue is polled
 *       on non-cuse thread.
 */
static void
cuse_cmd_done(void *_done, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_nvme_cpl *cuse_cpl = _done;
	struct iovec out_iov[2];

	cuse_cpl->cpl = *cpl;
	cuse_cpl->done = true;

	out_iov[0].iov_base = &cuse_cpl->cpl.cdw0;
	out_iov[0].iov_len = sizeof(cuse_cpl->cpl.cdw0);
	if (cuse_cpl->data_len > 0) {
		out_iov[1].iov_base = cuse_cpl->buf;
		out_iov[1].iov_len = cuse_cpl->data_len;
		fuse_reply_ioctl_iov(cuse_cpl->req, 0, out_iov, 2);
		spdk_free(cuse_cpl->buf);
	} else {
		fuse_reply_ioctl_iov(cuse_cpl->req, 0, out_iov, 1);
	}

	free(cuse_cpl);
}

static void
nvme_admin_cmd(fuse_req_t req, int cmd, void *arg,
	       struct fuse_file_info *fi, unsigned flags,
	       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct spdk_nvme_ctrlr *ctrlr = fuse_req_userdata(req);
	struct nvme_admin_cmd *admin_cmd;
	struct iovec in_iov, out_iov[2];
	struct spdk_nvme_cmd nvme_cmd;
	struct cuse_nvme_cpl *cuse_cpl;

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

		cuse_cpl = (struct cuse_nvme_cpl *)calloc(1, sizeof(struct cuse_nvme_cpl));
		cuse_cpl->req = req;
		cuse_cpl->done = false;
		cuse_cpl->data_len = admin_cmd->data_len;
		if (cuse_cpl->data_len > 0) {
			cuse_cpl->buf = spdk_malloc(cuse_cpl->data_len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		}
		spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &nvme_cmd, cuse_cpl->buf, cuse_cpl->data_len,
					      cuse_cmd_done, (void *)cuse_cpl);

		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		fuse_reply_err(req, EINVAL);
		break;
	}
}

uint8_t __data[512];

static void
nvme_user_cmd(fuse_req_t req, int cmd, void *arg,
	      struct fuse_file_info *fi, unsigned flags,
	      const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct spdk_nvme_ctrlr *ctrlr = fuse_req_userdata(req);
	struct nvme_passthru_cmd *passthru_io = in_buf;
	struct iovec in_iov[2], out_iov, iov;

	printf(">>> nvme_user_cmd\n");

	fuse_reply_err(req, EINVAL);
}

static void
nvme_submit_io(fuse_req_t req, int cmd, void *arg,
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
		break;
	default:
		fprintf(stderr, "SUBMIT_IO: opc:%d not valid\n", user_io->opcode);
		fuse_reply_err(req, EINVAL);
		return;
	}

}

static void
blkpbszget(fuse_req_t req, int cmd, void *arg,
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

	struct spdk_nvme_ns *ns = fuse_req_userdata(req);

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

	printf("addr=0x%jx\n", (uint64_t)arg);
	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(size);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct spdk_nvme_ns *ns = fuse_req_userdata(req);

	/* FIXIT: return size in 512 bytes blocks, not sectors! */
	size = spdk_nvme_ns_get_num_sectors(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
}

static void
blkgetsize64(fuse_req_t req, int cmd, void *arg,
	     struct fuse_file_info *fi, unsigned flags,
	     const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	uint64_t size;
	struct iovec out_iov;

	printf("addr=0x%jx\n", (uint64_t)arg);
	out_iov.iov_base = (void *)arg;
	out_iov.iov_len = sizeof(size);
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
		return;
	}

	struct spdk_nvme_ns *ns = fuse_req_userdata(req);

	size = spdk_nvme_ns_get_num_sectors(ns);
	fuse_reply_ioctl(req, 0, &size, sizeof(size));
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

	printf(">>> CMD 0x%X\n", cmd);
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		printf("NVME_IOCTL_ADMIN_CMD\n");
		nvme_admin_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case NVME_IOCTL_IO_CMD:
		printf("NVME_IOCTL_IO_CMD\n");
		nvme_user_cmd(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		printf("cmd=0x%x\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

static void
cuse_ns_ioctl(fuse_req_t req, int cmd, void *arg,
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

	case BLKROGET:
		printf("BLKROGET\n");
		fuse_reply_err(req, EINVAL);
		break;

	case BLKRAGET:
		printf("BLKRAGET\n");
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
		/* Returns the device size as a number of 512-byte blocks (returns pointer to long) */
		printf("BLKGETSIZE\n");
		blkgetsize(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	case BLKGETSIZE64:
		/* Returns the device size in sectors (returns pointer to uint64_t) */
		printf("BLKGETSIZE64\n");
		blkgetsize64(req, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);
		break;

	default:
		if ((cmd & 0xFFFFFF00) == 0x00001200) {
			printf("BLK IOCTL %2X\n", cmd & 0xFF);
			switch (cmd) {
			case BLKRRPART:
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

static void
cuse_ctrlr_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi)
{
	struct spdk_nvme_ctrlr *ctrlr = fuse_req_userdata(req);

	printf("... cuse_ctrlr_read: size %d, off %d\n", size, off);
	/* fuse_reply_buf(req, cusexmp_buf + off, size); */
	fuse_reply_err(req, EINVAL);
}

static void
cuse_ctrlr_write(fuse_req_t req, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	struct spdk_nvme_ctrlr *ctrlr = fuse_req_userdata(req);
	int i;

	printf("... cuse_ctrlr_write: size %d, off %d\n", size, off);
	for (i = 0; i < size; i++) {
		printf("%02X ", buf[i]);
	}
	printf("\n");

	/* Just a stub for testing */
	fuse_reply_write(req, size);

	/* fuse_reply_err(req, EINVAL); */
}

static void
cuse_ctrlr_flush(fuse_req_t req, struct fuse_file_info *fi)
{
	struct spdk_nvme_ctrlr *ctrlr = fuse_req_userdata(req);

	printf("... cuse_ctrlr_flush ...\n");
	fuse_reply_err(req, EINVAL);
}

static const struct cuse_lowlevel_ops cuse_ctrlr_clop = {
	.open		= cuse_open,
	.read		= cuse_ctrlr_read,
	.write		= cuse_ctrlr_write,
	.flush		= cuse_ctrlr_flush,
	.ioctl		= cuse_ctrlr_ioctl,
};


static void
read_complete(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_nvme_io_request *io = (struct cuse_nvme_io_request *)ref;

	/* Just a stub for testing */
	spdk_nvme_ctrlr_free_io_qpair(io->qpair);
	io->is_completed = true;

	fuse_reply_buf(io->req, io->buf + io->off, io->size);
	spdk_free(io->buf);
}

static void
cuse_ns_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi)
{
	int i;
	int rc;
	char *buf;

	struct cuse_nvme_io_request *io = (struct cuse_nvme_io_request *)malloc(sizeof(struct cuse_nvme_io_request));
	io->ns = fuse_req_userdata(req);
	io->ctrlr = io->ns->ctrlr;

	uint32_t block_size = spdk_nvme_ns_get_sector_size(io->ns);
	io->req = req;
	io->size = size;

	printf("Block size = %d\n", block_size);
	printf("... cuse_ns_read: size %d, off %d\n", size, off);

	io->lba_count = size / block_size;
	if (size % block_size) {
		io->lba_count++;
	}

	io->lba = off / block_size;
	io->off = off - io->lba * block_size;

	io->qpair = spdk_nvme_ctrlr_alloc_io_qpair(io->ctrlr, NULL, 0);
	if (io->qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return;
	}

	io->buf = spdk_nvme_ctrlr_alloc_cmb_io_buffer(io->ctrlr, io->lba_count * block_size);
	if (io->buf == NULL) {
		io->buf = spdk_zmalloc(io->lba_count * block_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}
	if (io->buf == NULL) {
		printf("ERROR: read buffer allocation failed\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	io->data = NULL;

	rc = spdk_nvme_ns_cmd_read(io->ns, io->qpair, io->buf,
				    io->lba, /* LBA start */
				    io->lba_count, /* number of LBAs */
				    read_complete, io, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("writev failed: rc = %d\n", rc);
		fuse_reply_err(req, EINVAL);
		return;
	}

#if 1
	io->is_completed = false;
	while (!io->is_completed) {
		spdk_nvme_qpair_process_completions(io->qpair, 0);
	}
#endif
}

static void
write_complete(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_nvme_io_request *io = (struct cuse_nvme_io_request *)ref;

	spdk_nvme_ctrlr_free_io_qpair(io->qpair);

	/* Just a stub for testing */
	io->is_completed = true;

	fuse_reply_write(io->req, io->size);
	spdk_free(io->buf);
}

static void
write_read_complete(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct cuse_nvme_io_request *io = (struct cuse_nvme_io_request *)ref;
	int rc;

	memcpy(io->buf, io->data, io->size);

	rc = spdk_nvme_ns_cmd_write(io->ns, io->qpair, io->buf,
				    io->lba, /* LBA start */
				    io->lba_count, /* number of LBAs */
				    write_complete, io, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("writev failed: rc = %d\n", rc);
		fuse_reply_err(io->req, EINVAL);
		return;
	}
}

static void
cuse_ns_write(fuse_req_t req, const char *_buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	int i;
	int rc;
	char *buf;

	printf("... cuse_ns_write: size %d, off %d\n", size, off);
	for (i = 0; i < size; i++) {
		printf("%02X ", _buf[i]);
	}
	printf("\n");


	struct cuse_nvme_io_request *io = (struct cuse_nvme_io_request *)malloc(sizeof(struct cuse_nvme_io_request));
	io->ns = fuse_req_userdata(req);
	io->ctrlr = io->ns->ctrlr;

	uint32_t block_size = spdk_nvme_ns_get_sector_size(io->ns);

	io->req = req;
	io->size = size;
	io->data = _buf;

	printf("Block size = %d\n", block_size);

	io->lba_count = size / block_size;
	if (size % block_size) {
		io->lba_count++;
	}

	io->lba = off / block_size;
	io->off = off - io->lba * block_size;

	io->qpair = spdk_nvme_ctrlr_alloc_io_qpair(io->ctrlr, NULL, 0);
	if (io->qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return;
	}

	io->buf = spdk_nvme_ctrlr_alloc_cmb_io_buffer(io->ctrlr, io->lba_count * block_size);
	if (io->buf == NULL) {
		io->buf = spdk_zmalloc(io->lba_count * block_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}
	if (io->buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		fuse_reply_err(req, ENOMEM);
		return;
	}

	rc = spdk_nvme_ns_cmd_read(io->ns, io->qpair, io->buf,
				    io->lba, /* LBA start */
				    io->lba_count, /* number of LBAs */
				    write_read_complete, io, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("read failed: rc = %d\n", rc);
		fuse_reply_err(req, EINVAL);
		return;
	}

#if 1
	io->is_completed = false;
	while (!io->is_completed) {
		spdk_nvme_qpair_process_completions(io->qpair, 0);
	}
#endif
}

static void
cuse_ns_flush(fuse_req_t req, struct fuse_file_info *fi)
{
	struct spdk_nvme_ns *ns = fuse_req_userdata(req);

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

static void *
cuse_ctrlr_thread(void *arg)
{
	struct spdk_nvme_ctrlr *ctrlr = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	char dev_name[128];
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;
	int multithreaded;

	snprintf(dev_name, sizeof(dev_name), "DEVNAME=nvme%d\n", ctrlr->cuse_device.idx);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	ctrlr->cuse_device.session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ctrlr_clop,
				     &multithreaded, ctrlr);
	fuse_session_loop(ctrlr->cuse_device.session);

	pthread_exit(NULL);
}

static void *
cuse_ns_thread(void *arg)
{
	struct spdk_nvme_ns *ns = arg;
	char *cuse_argv[] = { "cuse", "-f" };
	int cuse_argc = SPDK_COUNTOF(cuse_argv);
	char dev_name[128];
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;
	int multithreaded;

	snprintf(dev_name, sizeof(dev_name), "DEVNAME=nvme%dn%d\n", ns->ctrlr->cuse_device.idx,
		 ns->cuse_device.idx);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	ns->cuse_device.session = cuse_lowlevel_setup(cuse_argc, cuse_argv, &ci, &cuse_ns_clop,
				  &multithreaded, ns);
	fuse_session_loop(ns->cuse_device.session);

	pthread_exit(NULL);
}

int
spdk_nvme_cuse_add_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	ns->cuse_device.idx = nsid;

	if (pthread_create(&ns->cuse_device.tid, NULL, cuse_ns_thread, ns)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}

	return 0;
}

int
spdk_nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nsid;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Starting cuse devices for SSD: %s\n",
		      ctrlr->trid.traddr);

	if (!ctrlr->opts.enable_cuse_devices) {
		return 0;
	}

	ctrlr->cuse_device.idx = g_controllers_found++;

#if 0
	struct sigaction sigact;
	sigset_t sigmask;

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

	if (pthread_create(&ctrlr->cuse_device.tid, NULL, cuse_ctrlr_thread, ctrlr)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}

	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		nsid = i + 1;
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}
		spdk_nvme_cuse_add_ns(ctrlr, nsid);
	}

#if 0
	/* Now unmask SIGINT and SIGTERM for the main thread so that the
	 *  shutdown signal doesn't get sent to one of the pthreads.
	 */
	sigaddset(&sigmask, SIGHUP);
	sigdelset(&sigmask, SIGINT);
	sigdelset(&sigmask, SIGTERM);
	pthread_sigmask(SIG_SETMASK, &sigmask, NULL);
#endif

	return 0;
}

static int
spdk_nvme_cuse_device_stop(struct spdk_nvme_cuse_device *cuse_device)
{
	fuse_session_exit(cuse_device->session);
	pthread_kill(cuse_device->tid, SIGHUP);
	pthread_join(cuse_device->tid, NULL);
	return 0;
}

int
spdk_nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_ns *ns;
	uint32_t i;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Stopping cuse devices for SSD: %s\n",
		      ctrlr->trid.traddr);

	spdk_nvme_cuse_device_stop(&ctrlr->cuse_device);
	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		printf("Stopping ns %s\n", i);
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, i);
		spdk_nvme_cuse_device_stop(&ns->cuse_device);
	}

	return 0;
}
