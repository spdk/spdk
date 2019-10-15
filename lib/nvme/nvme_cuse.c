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

static void
cuse_ioctl(fuse_req_t req, int cmd, void *arg,
	   struct fuse_file_info *fi, unsigned flags,
	   const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	SPDK_ERRLOG("Unsupported IOCTL 0x%X.\n", cmd);
	fuse_reply_err(req, EINVAL);
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

static int
spdk_nvme_cuse_ns_start(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	struct cuse_device *ns_device;
	struct cuse_device *ctrlr_device;

	TAILQ_FOREACH(ctrlr_device, &g_ctrlr_ctx_head, tailq) {
		if (ctrlr_device->ctrlr == ctrlr) {
			break;
		}
	}

	if (!ctrlr_device) {
		SPDK_ERRLOG("Cannot find associated CUSE device\n");
		return -1;
	}

	ns_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	ns_device->ctrlr = ctrlr;
	ns_device->ctrlr_device = ctrlr_device;
	ns_device->idx = nsid;
	ns_device->nsid = nsid;
	snprintf(ns_device->dev_name, sizeof(ns_device->dev_name), "DEVNAME=spdk/nvme%dn%d\n",
		 ctrlr_device->idx, ns_device->idx);

	if (pthread_create(&ns_device->tid, NULL, cuse_thread, ns_device)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}

	TAILQ_INSERT_TAIL(&ctrlr_device->ns_devices, ns_device, tailq);
	return 0;
}

static int
spdk_nvme_cuse_start(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i, nsid;
	struct cuse_device *ctrlr_device;

	if (!ctrlr->opts.enable_cuse_devices) {
		return 0;
	}

	ctrlr_device = (struct cuse_device *)calloc(1, sizeof(struct cuse_device));
	TAILQ_INIT(&ctrlr_device->ns_devices);
	ctrlr_device->ctrlr = ctrlr;
	ctrlr_device->idx = g_controllers_found++;
	snprintf(ctrlr_device->dev_name, sizeof(ctrlr_device->dev_name),
		 "DEVNAME=spdk/nvme%d\n", ctrlr_device->idx);

	if (pthread_create(&ctrlr_device->tid, NULL, cuse_thread, ctrlr_device)) {
		SPDK_ERRLOG("pthread_create failed\n");
	}
	TAILQ_INSERT_TAIL(&g_ctrlr_ctx_head, ctrlr_device, tailq);

	/* Start all active namespaces */
	for (i = 0; i < spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
		nsid = i + 1;
		if (!spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid)) {
			continue;
		}

		spdk_nvme_cuse_ns_start(ctrlr, nsid);
	}

	return 0;
}

static int
spdk_nvme_cuse_stop(struct spdk_nvme_ctrlr *ctrlr)
{
	struct cuse_device *ctrlr_device;
	struct cuse_device *ns_device, *tmp;

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

	return 0;
}

static struct spdk_nvme_io_msg_producer cuse_nvme_io_msg_producer = {
	.name = "cuse",

	.ctrlr_start = spdk_nvme_cuse_start,
	.ctrlr_stop = spdk_nvme_cuse_stop,
};

SPDK_NVME_IO_MSG_REGISTER(cuse, &cuse_nvme_io_msg_producer);
