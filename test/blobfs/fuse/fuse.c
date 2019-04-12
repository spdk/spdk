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
#include <fuse/fuse_opt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <linux/ioctl.h>
#include <linux/nvme_ioctl.h>
// #include "ioctl.h"

static const char *usage =
"usage: cusexmp [options]\n"
"\n"
"options:\n"
"    --help|-h             print this help message\n"
"    --maj=MAJ|-M MAJ      device major number\n"
"    --min=MIN|-m MIN      device minor number\n"
"    --name=NAME|-n NAME   device name (mandatory)\n"
"    -d   -o debug         enable debug output (implies -f)\n"
"    -f                    foreground operation\n"
"    -s                    disable multi-threaded operation\n"
"\n";

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
	printf("addr=0x%jx\n", admin_cmd->addr);
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

struct cusexmp_param {
	unsigned		major;
	unsigned		minor;
	char			*dev_name;
	int			is_help;
};

#define CUSEXMP_OPT(t, p) { t, offsetof(struct cusexmp_param, p), 1 }

static const struct fuse_opt cusexmp_opts[] = {
	CUSEXMP_OPT("-M %u",		major),
	CUSEXMP_OPT("--maj=%u",		major),
	CUSEXMP_OPT("-m %u",		minor),
	CUSEXMP_OPT("--min=%u",		minor),
	CUSEXMP_OPT("-n %s",		dev_name),
	CUSEXMP_OPT("--name=%s",	dev_name),
	FUSE_OPT_KEY("-h",		0),
	FUSE_OPT_KEY("--help",		0),
	FUSE_OPT_END
};

static int cusexmp_process_arg(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct cusexmp_param *param = data;

	(void)outargs;
	(void)arg;

	switch (key) {
	case 0:
		param->is_help = 1;
		fprintf(stderr, "%s", usage);
		return fuse_opt_add_arg(outargs, "-ho");
	default:
		return 1;
	}
}

static const struct cuse_lowlevel_ops cusexmp_clop = {
	.open		= cusexmp_open,
	.ioctl		= cusexmp_ioctl,
};

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct cusexmp_param param = { 0, 0, NULL, 0 };
	char dev_name[128] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	struct cuse_info ci;

	if (fuse_opt_parse(&args, &param, cusexmp_opts, cusexmp_process_arg)) {
		printf("failed to parse option\n");
		return 1;
	}

	if (!param.is_help) {
		if (!param.dev_name) {
			fprintf(stderr, "Error: device name missing\n");
			return 1;
		}
		strncat(dev_name, param.dev_name, sizeof(dev_name) - 9);
	}

	memset(&ci, 0, sizeof(ci));
	ci.dev_major = param.major;
	ci.dev_minor = param.minor;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	return cuse_lowlevel_main(args.argc, args.argv, &ci, &cusexmp_clop,
				  NULL);
}
