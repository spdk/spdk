/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include <accel-config/libaccel_config.h>

#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/likely.h"

#include "spdk/log.h"
#include "spdk_internal/idxd.h"

#include "idxd_internal.h"

struct spdk_kernel_idxd_device {
	struct spdk_idxd_device	idxd;
	struct accfg_ctx	*ctx;

	unsigned int		max_batch_size;
	unsigned int		max_xfer_size;
	unsigned int		max_xfer_bits;

	/* We only use a single WQ */
	struct accfg_wq		*wq;
	int			fd;
	void			*portal;
};

#define __kernel_idxd(idxd) SPDK_CONTAINEROF(idxd, struct spdk_kernel_idxd_device, idxd)

static void
kernel_idxd_device_destruct(struct spdk_idxd_device *idxd)
{
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);

	if (kernel_idxd->portal != NULL) {
		munmap(kernel_idxd->portal, 0x1000);
	}

	if (kernel_idxd->fd >= 0) {
		close(kernel_idxd->fd);
	}

	accfg_unref(kernel_idxd->ctx);
	free(kernel_idxd);
}

static struct spdk_idxd_impl g_kernel_idxd_impl;

static int
kernel_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb, spdk_idxd_probe_cb probe_cb)
{
	int rc;
	struct accfg_ctx *ctx;
	struct accfg_device *device;

	rc = accfg_new(&ctx);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate accel-config context\n");
		return rc;
	}

	/* Loop over each IDXD device */
	accfg_device_foreach(ctx, device) {
		enum accfg_device_state dstate;
		struct spdk_kernel_idxd_device *kernel_idxd;
		struct accfg_wq *wq;
		bool pasid_enabled;

		/* Make sure that the device is enabled */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED) {
			continue;
		}

		pasid_enabled = accfg_device_get_pasid_enabled(device);
		if (!pasid_enabled && spdk_iommu_is_enabled()) {
			/*
			 * If the IOMMU is enabled but shared memory mode is not on,
			 * then we have no way to get the IOVA from userspace to use this
			 * device or any kernel device. Return an error.
			 */
			SPDK_ERRLOG("Found kernel IDXD device, but cannot use it when IOMMU is enabled but SM is disabled\n");
			return -ENOTSUP;
		}

		kernel_idxd = calloc(1, sizeof(struct spdk_kernel_idxd_device));
		if (kernel_idxd == NULL) {
			SPDK_ERRLOG("Failed to allocate memory for kernel_idxd device.\n");
			/* TODO: Goto error cleanup */
			return -ENOMEM;
		}

		kernel_idxd->max_batch_size = accfg_device_get_max_batch_size(device);
		kernel_idxd->max_xfer_size = accfg_device_get_max_transfer_size(device);
		kernel_idxd->idxd.socket_id = accfg_device_get_numa_node(device);
		kernel_idxd->idxd.impl = &g_kernel_idxd_impl;
		kernel_idxd->fd = -1;
		kernel_idxd->idxd.version = accfg_device_get_version(device);
		kernel_idxd->idxd.pasid_enabled = pasid_enabled;

		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;
			enum accfg_wq_mode mode;
			enum accfg_wq_type type;
			int major, minor;
			char path[1024];

			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED) {
				continue;
			}

			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER) {
				continue;
			}

			/* TODO: For now, only support dedicated WQ */
			mode = accfg_wq_get_mode(wq);
			if (mode != ACCFG_WQ_DEDICATED) {
				continue;
			}

			major = accfg_device_get_cdev_major(device);
			if (major < 0) {
				continue;
			}

			minor = accfg_wq_get_cdev_minor(wq);
			if (minor < 0) {
				continue;
			}

			/* Map the portal */
			snprintf(path, sizeof(path), "/dev/char/%u:%u", major, minor);
			kernel_idxd->fd = open(path, O_RDWR);
			if (kernel_idxd->fd < 0) {
				SPDK_ERRLOG("Can not open the WQ file descriptor on path=%s\n",
					    path);
				continue;
			}

			kernel_idxd->portal = mmap(NULL, 0x1000, PROT_WRITE,
						   MAP_SHARED | MAP_POPULATE, kernel_idxd->fd, 0);
			if (kernel_idxd->portal == MAP_FAILED) {
				perror("mmap");
				continue;
			}

			kernel_idxd->wq = wq;

			/* Since we only use a single WQ, the total size is the size of this WQ */
			kernel_idxd->idxd.total_wq_size = accfg_wq_get_size(wq);
			kernel_idxd->idxd.chan_per_device = (kernel_idxd->idxd.total_wq_size >= 128) ? 8 : 4;

			/* We only use a single WQ, so once we've found one we can stop looking. */
			break;
		}

		if (kernel_idxd->idxd.total_wq_size > 0) {
			/* This device has at least 1 WQ available, so ask the user if they want to use it. */
			attach_cb(cb_ctx, &kernel_idxd->idxd);
		} else {
			kernel_idxd_device_destruct(&kernel_idxd->idxd);
		}
	}

	return 0;
}

static void
kernel_idxd_dump_sw_error(struct spdk_idxd_device *idxd, void *portal)
{
	/* Need to be enhanced later */
}

static char *
kernel_idxd_portal_get_addr(struct spdk_idxd_device *idxd)
{
	struct spdk_kernel_idxd_device *kernel_idxd = __kernel_idxd(idxd);

	return kernel_idxd->portal;
}

static struct spdk_idxd_impl g_kernel_idxd_impl = {
	.name			= "kernel",
	.probe			= kernel_idxd_probe,
	.destruct		= kernel_idxd_device_destruct,
	.dump_sw_error		= kernel_idxd_dump_sw_error,
	.portal_get_addr	= kernel_idxd_portal_get_addr,
};

SPDK_IDXD_IMPL_REGISTER(kernel, &g_kernel_idxd_impl);
