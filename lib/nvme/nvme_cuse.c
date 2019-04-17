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
#define _FILE_OFFSET_BITS 64

#include <fuse/cuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <linux/ioctl.h>
#include <linux/nvme_ioctl.h>

#include <spdk/util.h>
#include "nvme_internal.h"

static void cusexmp_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

static void cusexmp_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	const struct nvme_admin_cmd *admin_cmd = in_buf;
	char outbuf[4096];
	struct iovec in_iov, out_iov, iov;

	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	printf("in_bufsz = %d\n", (int)in_bufsz);
	printf("out_bufsz = %d\n", (int)out_bufsz);
	printf("arg=%p\n", arg);
	in_iov.iov_base = arg;
	in_iov.iov_len = sizeof(*admin_cmd);
	if (in_bufsz == 0) {
		fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
		return;
	}

	printf("in_buf=%p\n", in_buf);
	printf("addr=0x%" PRIx64 "\n", (uint64_t)admin_cmd->addr);
	out_iov.iov_base = (void *)admin_cmd->addr;
	out_iov.iov_len = 4096;
	if (out_bufsz == 0) {
		fuse_reply_ioctl_retry(req, &in_iov, 1, &out_iov, 1);
		return;
	}

	memset(outbuf, 0, sizeof(outbuf));
	outbuf[0] = 0xFF;
	outbuf[1] = 0xEE;
	iov.iov_base = outbuf;
	iov.iov_len = sizeof(outbuf);
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		printf("NVME_IOCTL_ADMIN_CMD\n");
		fuse_reply_ioctl_iov(req, 0, &iov, 1);
		//fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	default:
		printf("cmd=0x%x\n", cmd);
		fuse_reply_err(req, EINVAL);
	}
}

static const struct cuse_lowlevel_ops cusexmp_clop = {
	.open		= cusexmp_open,
	.ioctl		= cusexmp_ioctl,
};

int nvme_cuse_start(char *dev_name)
{
	char *argv[] = { "-f" };
	int argc = SPDK_COUNTOF(argv);
	char dev_name_str[128] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name_str };
	struct cuse_info ci;

	strncat(dev_name_str, dev_name, sizeof(dev_name_str) - 9);

	memset(&ci, 0, sizeof(ci));
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	return cuse_lowlevel_main(argc, argv, &ci, &cusexmp_clop, NULL);
}
