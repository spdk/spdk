/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"
#include <rte_version.h>

#include "spdk_internal/vhost_user.h"

char g_vhost_user_dev_dirname[PATH_MAX] = "";
sem_t g_dpdk_sem;

static void __attribute__((constructor))
_vhost_user_sem_init(void)
{
	if (sem_init(&g_dpdk_sem, 0, 0) != 0) {
		SPDK_ERRLOG("Failed to initialize semaphore for rte_vhost pthread.\n");
		abort();
	}
}

static void __attribute__((destructor))
_vhost_user_sem_destroy(void)
{
	sem_destroy(&g_dpdk_sem);
}

static inline void
vhost_session_mem_region_calc(uint64_t *previous_start, uint64_t *start, uint64_t *end,
			      uint64_t *len, struct rte_vhost_mem_region *region)
{
	*start = FLOOR_2MB(region->mmap_addr);
	*end = CEIL_2MB(region->mmap_addr + region->mmap_size);
	if (*start == *previous_start) {
		*start += (size_t) VALUE_2MB;
	}
	*previous_start = *start;
	*len = *end - *start;
}

void
vhost_session_mem_register(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;


	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		SPDK_INFOLOG(vhost, "Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			     start, len);

		if (spdk_mem_register((void *)start, len) != 0) {
			SPDK_WARNLOG("Failed to register memory region %"PRIu32". Future vtophys translation might fail.\n",
				     i);
			continue;
		}
	}
}

void
vhost_session_mem_unregister(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;

	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		if (spdk_vtophys((void *) start, NULL) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
		}

		if (spdk_mem_unregister((void *)start, len) != 0) {
			assert(false);
		}
	}
}

static int
new_connection(int vid)
{
	char ifname[PATH_MAX];

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		return -1;
	}

	return vhost_new_connection_cb(vid, ifname);
}

static int
start_device(int vid)
{
	return vhost_start_device_cb(vid);
}

static void
stop_device(int vid)
{
	vhost_stop_device_cb(vid);
}

static void
destroy_connection(int vid)
{
	vhost_destroy_connection_cb(vid);
}

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
static const struct rte_vhost_device_ops g_spdk_vhost_ops = {
#else
static const struct vhost_device_ops g_spdk_vhost_ops = {
#endif
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
};

static enum rte_vhost_msg_result
extern_vhost_pre_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	switch (msg->request) {
	case VHOST_USER_GET_VRING_BASE:
		if (vsession->forced_polling && vsession->started) {
			/* Our queue is stopped for whatever reason, but we may still
			 * need to poll it after it's initialized again.
			 */
			g_spdk_vhost_ops.destroy_device(vid);
		}
		break;
	case VHOST_USER_SET_VRING_BASE:
	case VHOST_USER_SET_VRING_ADDR:
	case VHOST_USER_SET_VRING_NUM:
		/* For vhost-user socket messages except VHOST_USER_GET_VRING_BASE,
		 * rte_vhost holds all VQ's access lock, then after DPDK 22.07 release,
		 * `rte_vhost_vring_call` also needs to hold VQ's access lock, so we
		 * can't call this function in DPDK "vhost-events" thread context, here
		 * SPDK vring poller will avoid executing this function when it's TRUE.
		 */
		vsession->skip_used_signal = true;
		if (vsession->forced_polling && vsession->started) {
			/* Additional queues are being initialized, so we either processed
			 * enough I/Os and are switching from SeaBIOS to the OS now, or
			 * we were never in SeaBIOS in the first place. Either way, we
			 * don't need our workaround anymore.
			 */
			g_spdk_vhost_ops.destroy_device(vid);
			vsession->forced_polling = false;
		}
		break;
	case VHOST_USER_SET_VRING_KICK:
		/* rte_vhost(after 20.08) will call new_device after one active vring is
		 * configured, we will start the session before all vrings are available,
		 * so for each new vring, if the session is started, we need to restart it
		 * again.
		 */
	case VHOST_USER_SET_VRING_CALL:
		/* rte_vhost will close the previous callfd and won't notify
		 * us about any change. This will effectively make SPDK fail
		 * to deliver any subsequent interrupts until a session is
		 * restarted. We stop the session here before closing the previous
		 * fd (so that all interrupts must have been delivered by the
		 * time the descriptor is closed) and start right after (which
		 * will make SPDK retrieve the latest, up-to-date callfd from
		 * rte_vhost.
		 */
	case VHOST_USER_SET_MEM_TABLE:
		vsession->skip_used_signal = true;
		/* rte_vhost will unmap previous memory that SPDK may still
		 * have pending DMA operations on. We can't let that happen,
		 * so stop the device before letting rte_vhost unmap anything.
		 * This will block until all pending I/Os are finished.
		 * We will start the device again from the post-processing
		 * message handler.
		 */
		if (vsession->started) {
			g_spdk_vhost_ops.destroy_device(vid);
			vsession->needs_restart = true;
		}
		break;
	case VHOST_USER_GET_CONFIG: {
		int rc = 0;

		spdk_vhost_lock();
		if (vsession->vdev->backend->vhost_get_config) {
			rc = vsession->vdev->backend->vhost_get_config(vsession->vdev,
				msg->payload.cfg.region, msg->payload.cfg.size);
			if (rc != 0) {
				msg->size = 0;
			}
		}
		spdk_vhost_unlock();

		return RTE_VHOST_MSG_RESULT_REPLY;
	}
	case VHOST_USER_SET_CONFIG: {
		int rc = 0;

		spdk_vhost_lock();
		if (vsession->vdev->backend->vhost_set_config) {
			rc = vsession->vdev->backend->vhost_set_config(vsession->vdev,
				msg->payload.cfg.region, msg->payload.cfg.offset,
				msg->payload.cfg.size, msg->payload.cfg.flags);
		}
		spdk_vhost_unlock();

		return rc == 0 ? RTE_VHOST_MSG_RESULT_OK : RTE_VHOST_MSG_RESULT_ERR;
	}
	default:
		break;
	}

	vsession->skip_used_signal = false;
	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static enum rte_vhost_msg_result
extern_vhost_post_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to unitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}

	if (vsession->needs_restart) {
		g_spdk_vhost_ops.new_device(vid);
		vsession->needs_restart = false;
		return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
	}

	switch (msg->request) {
	case VHOST_USER_SET_FEATURES:
		/* rte_vhost requires all queues to be fully initialized in order
		 * to start I/O processing. This behavior is not compliant with the
		 * vhost-user specification and doesn't work with QEMU 2.12+, which
		 * will only initialize 1 I/O queue for the SeaBIOS boot.
		 * Theoretically, we should start polling each virtqueue individually
		 * after receiving its SET_VRING_KICK message, but rte_vhost is not
		 * designed to poll individual queues. So here we use a workaround
		 * to detect when the vhost session could be potentially at that SeaBIOS
		 * stage and we mark it to start polling as soon as its first virtqueue
		 * gets initialized. This doesn't hurt any non-QEMU vhost slaves
		 * and allows QEMU 2.12+ to boot correctly. SET_FEATURES could be sent
		 * at any time, but QEMU will send it at least once on SeaBIOS
		 * initialization - whenever powered-up or rebooted.
		 */
		vsession->forced_polling = true;
		break;
	case VHOST_USER_SET_VRING_KICK:
		/* vhost-user spec tells us to start polling a queue after receiving
		 * its SET_VRING_KICK message. Let's do it!
		 */
		if (vsession->forced_polling && !vsession->started) {
			g_spdk_vhost_ops.new_device(vid);
		}
		break;
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

struct rte_vhost_user_extern_ops g_spdk_extern_vhost_ops = {
	.pre_msg_handle = extern_vhost_pre_msg_handler,
	.post_msg_handle = extern_vhost_post_msg_handler,
};

void
vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
	int rc;

	rc = rte_vhost_extern_callback_register(vsession->vid, &g_spdk_extern_vhost_ops, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("rte_vhost_extern_callback_register() failed for vid = %d\n",
			    vsession->vid);
		return;
	}
}

int
vhost_register_unix_socket(const char *path, const char *ctrl_name,
			   uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features)
{
	struct stat file_stat;
	uint64_t features = 0;

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The file already exists and is not a socket.\n",
				    path);
			return -EIO;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The socket already exists and failed to unlink.\n",
				    path);
			return -EIO;
		}
	}

#if RTE_VERSION < RTE_VERSION_NUM(20, 8, 0, 0)
	if (rte_vhost_driver_register(path, 0) != 0) {
#else
	if (rte_vhost_driver_register(path, RTE_VHOST_USER_ASYNC_COPY) != 0) {
#endif
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", ctrl_name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}
	if (rte_vhost_driver_set_features(path, virtio_features) ||
	    rte_vhost_driver_disable_features(path, disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", ctrl_name);

		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	if (rte_vhost_driver_callback_register(path, &g_spdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", ctrl_name);
		return -EIO;
	}

	rte_vhost_driver_get_protocol_features(path, &features);
	features |= protocol_features;
	rte_vhost_driver_set_protocol_features(path, features);

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    ctrl_name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	return 0;
}

int
vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	return rte_vhost_get_mem_table(vid, mem);
}

int
vhost_driver_unregister(const char *path)
{
	return rte_vhost_driver_unregister(path);
}

int
vhost_get_negotiated_features(int vid, uint64_t *negotiated_features)
{
	return rte_vhost_get_negotiated_features(vid, negotiated_features);
}

int
vhost_user_dev_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			 uint32_t iops_threshold)
{
	uint64_t delay_time_base = delay_base_us * spdk_get_ticks_hz() / 1000000ULL;
	uint32_t io_rate = iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;

	if (delay_time_base >= UINT32_MAX) {
		SPDK_ERRLOG("Delay time of %"PRIu32" is to big\n", delay_base_us);
		return -EINVAL;
	} else if (io_rate == 0) {
		SPDK_ERRLOG("IOPS rate of %"PRIu32" is too low. Min is %u\n", io_rate,
			    1000U / SPDK_VHOST_STATS_CHECK_INTERVAL_MS);
		return -EINVAL;
	}

	vdev->coalescing_delay_us = delay_base_us;
	vdev->coalescing_iops_threshold = iops_threshold;
	return 0;
}

int
vhost_user_session_set_coalescing(struct spdk_vhost_dev *vdev,
			     struct spdk_vhost_session *vsession, void *ctx)
{
	vsession->coalescing_delay_time_base =
		vdev->coalescing_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	vsession->coalescing_io_rate_threshold =
		vdev->coalescing_iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;
	return 0;
}

int
spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	int rc;

	rc = vhost_user_dev_set_coalescing(vdev, delay_base_us, iops_threshold);
	if (rc != 0) {
		return rc;
	}

	vhost_dev_foreach_session(vdev, vhost_user_session_set_coalescing, NULL, NULL);
	return 0;
}

void
spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	if (delay_base_us) {
		*delay_base_us = vdev->coalescing_delay_us;
	}

	if (iops_threshold) {
		*iops_threshold = vdev->coalescing_iops_threshold;
	}
}

int
spdk_vhost_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
		if ((size_t)ret >= sizeof(g_vhost_user_dev_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (g_vhost_user_dev_dirname[ret - 1] != '/') {
			g_vhost_user_dev_dirname[ret] = '/';
			g_vhost_user_dev_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}
