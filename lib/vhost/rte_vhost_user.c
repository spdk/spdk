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

/* Path to folder where character device will be created. Can be set by user. */
static char g_vhost_user_dev_dirname[PATH_MAX] = "";
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
_stop_session(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_virtqueue *q;
	int rc;
	uint16_t i;

	rc = vdev->backend->stop_session(vsession);
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vsession->vid);
		return rc;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		q = &vsession->virtqueue[i];

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL) {
			continue;
		}

		/* Packed virtqueues support up to 2^15 entries each
		 * so left one bit can be used as wrap counter.
		 */
		if (q->packed.packed_ring) {
			q->last_avail_idx = q->last_avail_idx |
					    ((uint16_t)q->packed.avail_phase << 15);
			q->last_used_idx = q->last_used_idx |
					   ((uint16_t)q->packed.used_phase << 15);
		}

		rte_vhost_set_vring_base(vsession->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	vhost_session_mem_unregister(vsession->mem);
	free(vsession->mem);

	return 0;
}

static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	size_t dev_dirname_len;
	char ifname[PATH_MAX];
	char *ctrlr_name;

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		return -1;
	}

	spdk_vhost_lock();

	ctrlr_name = &ifname[0];
	dev_dirname_len = strlen(g_vhost_user_dev_dirname);
	if (strncmp(ctrlr_name, g_vhost_user_dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	vdev = spdk_vhost_dev_find(ctrlr_name);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to create connection for.\n", vid);
		spdk_vhost_unlock();
		return -1;
	}

	/* We expect sessions inside vdev->vsessions to be sorted in ascending
	 * order in regard of vsession->id. For now we always set id = vsessions_cnt++
	 * and append each session to the very end of the vsessions list.
	 * This is required for spdk_vhost_dev_foreach_session() to work.
	 */
	if (vdev->vsessions_num == UINT_MAX) {
		assert(false);
		return -EINVAL;
	}

	if (posix_memalign((void **)&vsession, SPDK_CACHE_LINE_SIZE, sizeof(*vsession) +
			   vdev->backend->session_ctx_size)) {
		SPDK_ERRLOG("vsession alloc failed\n");
		spdk_vhost_unlock();
		return -1;
	}
	memset(vsession, 0, sizeof(*vsession) + vdev->backend->session_ctx_size);

	vsession->vdev = vdev;
	vsession->vid = vid;
	vsession->id = vdev->vsessions_num++;
	vsession->name = spdk_sprintf_alloc("%ss%u", vdev->name, vsession->vid);
	if (vsession->name == NULL) {
		SPDK_ERRLOG("vsession alloc failed\n");
		spdk_vhost_unlock();
		free(vsession);
		return -1;
	}
	vsession->started = false;
	vsession->initialized = false;
	vsession->next_stats_check_time = 0;
	vsession->stats_check_interval = SPDK_VHOST_STATS_CHECK_INTERVAL_MS *
					 spdk_get_ticks_hz() / 1000UL;
	TAILQ_INSERT_TAIL(&vdev->vsessions, vsession, tailq);

	vhost_session_install_rte_compat_hooks(vsession);
	spdk_vhost_unlock();
	return 0;
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	int rc = -1;
	uint16_t i;
	bool packed_ring;

	spdk_vhost_lock();

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		goto out;
	}

	vdev = vsession->vdev;
	if (vsession->started) {
		/* already started, nothing to do */
		rc = 0;
		goto out;
	}

	if (vhost_get_negotiated_features(vid, &vsession->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		goto out;
	}

	packed_ring = ((vsession->negotiated_features & (1ULL << VIRTIO_F_RING_PACKED)) != 0);

	vsession->max_queues = 0;
	memset(vsession->virtqueue, 0, sizeof(vsession->virtqueue));
	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];

		q->vsession = vsession;
		q->vring_idx = -1;
		if (rte_vhost_get_vhost_vring(vid, i, &q->vring)) {
			continue;
		}
		q->vring_idx = i;
		rte_vhost_get_vhost_ring_inflight(vid, i, &q->vring_inflight);

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL || q->vring.size == 0) {
			continue;
		}

		if (rte_vhost_get_vring_base(vsession->vid, i, &q->last_avail_idx, &q->last_used_idx)) {
			q->vring.desc = NULL;
			continue;
		}

		if (packed_ring) {
			/* Use the inflight mem to restore the last_avail_idx and last_used_idx.
			 * When the vring format is packed, there is no used_idx in the
			 * used ring, so VM can't resend the used_idx to VHOST when reconnect.
			 * QEMU version 5.2.0 supports the packed inflight before that it only
			 * supports split ring inflight because it doesn't send negotiated features
			 * before get inflight fd. Users can use RPC to enable this function.
			 */
			if (spdk_unlikely(g_packed_ring_recovery)) {
				rte_vhost_get_vring_base_from_inflight(vsession->vid, i,
								       &q->last_avail_idx,
								       &q->last_used_idx);
			}

			/* Packed virtqueues support up to 2^15 entries each
			 * so left one bit can be used as wrap counter.
			 */
			q->packed.avail_phase = q->last_avail_idx >> 15;
			q->last_avail_idx = q->last_avail_idx & 0x7FFF;
			q->packed.used_phase = q->last_used_idx >> 15;
			q->last_used_idx = q->last_used_idx & 0x7FFF;

			if (!vsession->interrupt_mode) {
				/* Disable I/O submission notifications, we'll be polling. */
				q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
			}
		} else {
			if (!vsession->interrupt_mode) {
				/* Disable I/O submission notifications, we'll be polling. */
				q->vring.used->flags = VRING_USED_F_NO_NOTIFY;
			}
		}

		q->packed.packed_ring = packed_ring;
		vsession->max_queues = i + 1;
	}

	if (vhost_get_mem_table(vid, &vsession->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto out;
	}

	/*
	 * Not sure right now but this look like some kind of QEMU bug and guest IO
	 * might be frozed without kicking all queues after live-migration. This look like
	 * the previous vhost instance failed to effectively deliver all interrupts before
	 * the GET_VRING_BASE message. This shouldn't harm guest since spurious interrupts
	 * should be ignored by guest virtio driver.
	 *
	 * Tested on QEMU 2.10.91 and 2.11.50.
	 */
	for (i = 0; i < vsession->max_queues; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc != NULL && q->vring.size > 0) {
			rte_vhost_vring_call(vsession->vid, q->vring_idx);
		}
	}

	vhost_user_session_set_coalescing(vdev, vsession, NULL);
	vhost_session_mem_register(vsession->mem);
	vsession->initialized = true;
	rc = vdev->backend->start_session(vsession);
	if (rc != 0) {
		vhost_session_mem_unregister(vsession->mem);
		free(vsession->mem);
		goto out;
	}

out:
	spdk_vhost_unlock();
	return rc;
}

static void
stop_device(int vid)
{
	struct spdk_vhost_session *vsession;

	spdk_vhost_lock();
	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		spdk_vhost_unlock();
		return;
	}

	if (!vsession->started) {
		/* already stopped, nothing to do */
		spdk_vhost_unlock();
		return;
	}

	_stop_session(vsession);
	spdk_vhost_unlock();

	return;
}

static void
destroy_connection(int vid)
{
	struct spdk_vhost_session *vsession;

	spdk_vhost_lock();
	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		spdk_vhost_unlock();
		return;
	}

	if (vsession->started) {
		if (_stop_session(vsession) != 0) {
			spdk_vhost_unlock();
			return;
		}
	}

	TAILQ_REMOVE(&vsession->vdev->vsessions, vsession, tailq);
	free(vsession->name);
	free(vsession);
	spdk_vhost_unlock();
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

void
vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession, bool interrupt_mode)
{
	uint16_t i;
	bool packed_ring;
	int rc = 0;

	packed_ring = ((vsession->negotiated_features & (1ULL << VIRTIO_F_RING_PACKED)) != 0);

	for (i = 0; i < vsession->max_queues; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];
		uint64_t num_events = 1;

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL || q->vring.size == 0) {
			continue;
		}

		if (interrupt_mode) {
			/* Enable I/O submission notifications, we'll be interrupting. */
			if (packed_ring) {
				* (volatile uint16_t *) &q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_ENABLE;
			} else {
				* (volatile uint16_t *) &q->vring.used->flags = 0;
			}

			/* In case of race condition, always kick vring when switch to intr */
			rc = write(q->vring.kickfd, &num_events, sizeof(num_events));
			if (rc < 0) {
				SPDK_ERRLOG("failed to kick vring: %s.\n", spdk_strerror(errno));
			}

			vsession->interrupt_mode = true;
		} else {
			/* Disable I/O submission notifications, we'll be polling. */
			if (packed_ring) {
				* (volatile uint16_t *) &q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
			} else {
				* (volatile uint16_t *) &q->vring.used->flags = VRING_USED_F_NO_NOTIFY;
			}

			vsession->interrupt_mode = false;
		}
	}
}


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

static void
vhost_dev_thread_exit(void *arg1)
{
	spdk_thread_exit(spdk_get_thread());
}

int
vhost_user_dev_register(struct spdk_vhost_dev *vdev, const char *name, struct spdk_cpuset *cpumask,
			const struct spdk_vhost_dev_backend *backend)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s%s", g_vhost_user_dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n",
				name,g_vhost_user_dev_dirname, name);
		return -EINVAL;
	}

	vdev->path = strdup(path);
	if (vdev->path == NULL) {
		return -EIO;
	}

	vdev->thread = spdk_thread_create(vdev->name, cpumask);
	if (vdev->thread == NULL) {
		free(vdev->path);
		SPDK_ERRLOG("Failed to create thread for vhost controller %s.\n", name);
		return -EIO;
	}

	vdev->registered = true;
	vdev->backend = backend;
	TAILQ_INIT(&vdev->vsessions);

	vhost_user_dev_set_coalescing(vdev, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				 SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	if (vhost_register_unix_socket(path, name, vdev->virtio_features, vdev->disabled_features,
				       vdev->protocol_features)) {
		spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
		free(vdev->path);
		return -EIO;
	}

	return 0;
}

int
vhost_user_dev_unregister(struct spdk_vhost_dev *vdev)
{
	if (!TAILQ_EMPTY(&vdev->vsessions)) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		return -EBUSY;
	}

	if (vdev->registered && vhost_driver_unregister(vdev->path) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s with vhost library\n"
			    "Check if domain socket %s still exists\n",
			    vdev->name, vdev->path);
		return -EIO;
	}

	spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
	free(vdev->path);

	return 0;
}

static bool g_vhost_user_started = false;

int
vhost_user_init(void)
{
	size_t len;

	if (g_vhost_user_started) {
		return 0;
	}

	if (g_vhost_user_dev_dirname[0] == '\0') {
		if (getcwd(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 1) == NULL) {
			SPDK_ERRLOG("getcwd failed (%d): %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		len = strlen(g_vhost_user_dev_dirname);
		if (g_vhost_user_dev_dirname[len - 1] != '/') {
			g_vhost_user_dev_dirname[len] = '/';
			g_vhost_user_dev_dirname[len + 1] = '\0';
		}
	}

	g_vhost_user_started = true;

	return 0;
}

static void *
vhost_user_session_shutdown(void *arg)
{
	struct spdk_vhost_dev *vdev = NULL;
	struct spdk_vhost_session *vsession;
	vhost_fini_cb vhost_cb = arg;

	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		spdk_vhost_lock();
		TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
			if (vsession->started) {
				_stop_session(vsession);
			}
		}
		spdk_vhost_unlock();
		vhost_driver_unregister(vdev->path);
		vdev->registered = false;
	}

	SPDK_INFOLOG(vhost, "Exiting\n");
	spdk_thread_send_msg(g_vhost_init_thread, vhost_cb, NULL);
	return NULL;
}

void
vhost_user_fini(vhost_fini_cb vhost_cb)
{
	pthread_t tid;
	int rc;

	if (!g_vhost_user_started) {
		vhost_cb(NULL);
		return;
	}

	g_vhost_user_started = false;

	/* rte_vhost API for removing sockets is not asynchronous. Since it may call SPDK
	 * ops for stopping a device or removing a connection, we need to call it from
	 * a separate thread to avoid deadlock.
	 */
	rc = pthread_create(&tid, NULL, &vhost_user_session_shutdown, vhost_cb);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s\n", rc, spdk_strerror(rc));
		abort();
	}
	pthread_detach(tid);
}
