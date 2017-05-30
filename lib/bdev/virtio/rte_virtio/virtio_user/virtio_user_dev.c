/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
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

#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vhost.h"
#include "virtio_user_dev.h"
#include "../virtio_ethdev.h"

static int
virtio_user_create_queue(struct virtio_user_dev *dev, uint32_t queue_sel)
{
	/* Of all per virtqueue MSGs, make sure VHOST_SET_VRING_CALL come
	 * firstly because vhost depends on this msg to allocate virtqueue
	 * pair.
	 */
	struct vhost_vring_file file;

	file.index = queue_sel;
	file.fd = dev->callfds[queue_sel];
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_CALL, &file);

	return 0;
}

static int
virtio_user_kick_queue(struct virtio_user_dev *dev, uint32_t queue_sel)
{
	struct vhost_vring_file file;
	struct vhost_vring_state state;
	struct vring *vring = &dev->vrings[queue_sel];
	struct vhost_vring_addr addr = {
		.index = queue_sel,
		.desc_user_addr = (uint64_t)(uintptr_t)vring->desc,
		.avail_user_addr = (uint64_t)(uintptr_t)vring->avail,
		.used_user_addr = (uint64_t)(uintptr_t)vring->used,
		.log_guest_addr = 0,
		.flags = 0, /* disable log */
	};

	state.index = queue_sel;
	state.num = vring->num;
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_NUM, &state);

	state.index = queue_sel;
	state.num = 0; /* no reservation */
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_BASE, &state);

	dev->ops->send_request(dev, VHOST_USER_SET_VRING_ADDR, &addr);

	/* Of all per virtqueue MSGs, make sure VHOST_USER_SET_VRING_KICK comes
	 * lastly because vhost depends on this msg to judge if
	 * virtio is ready.
	 */
	file.index = queue_sel;
	file.fd = dev->kickfds[queue_sel];
	dev->ops->send_request(dev, VHOST_USER_SET_VRING_KICK, &file);

	return 0;
}

static int
virtio_user_queue_setup(struct virtio_user_dev *dev,
			int (*fn)(struct virtio_user_dev *, uint32_t))
{
	uint32_t i;

	for (i = 0; i < dev->max_queues; ++i) {
		if (fn(dev, i) < 0) {
			PMD_DRV_LOG(INFO, "setup tx vq fails: %u", i);
			return -1;
		}
	}

	return 0;
}

int
virtio_user_start_device(struct virtio_user_dev *dev)
{
	uint64_t features;
	int ret;

	/* Step 0: tell vhost to create queues */
	if (virtio_user_queue_setup(dev, virtio_user_create_queue) < 0)
		goto error;

	/* Step 1: set features */
	features = dev->features;

	printf("features = %jx\n", features);

	ret = dev->ops->send_request(dev, VHOST_USER_SET_FEATURES, &features);
	if (ret < 0)
		goto error;
	PMD_DRV_LOG(INFO, "set features: %" PRIx64, features);

	/* Step 2: share memory regions */
	ret = dev->ops->send_request(dev, VHOST_USER_SET_MEM_TABLE, NULL);
	if (ret < 0)
		goto error;

	/* Step 3: kick queues */
	if (virtio_user_queue_setup(dev, virtio_user_kick_queue) < 0)
		goto error;

	return 0;
error:
	/* TODO: free resource here or caller to check */
	return -1;
}

int virtio_user_stop_device(struct virtio_user_dev *dev)
{
	return 0;
}

int
is_vhost_user_by_type(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) == -1)
		return 0;

	return S_ISSOCK(sb.st_mode);
}

static int
virtio_user_dev_init_notify(struct virtio_user_dev *dev)
{
	uint32_t i, j;
	int callfd;
	int kickfd;

	for (i = 0; i < VIRTIO_MAX_VIRTQUEUES; ++i) {
		if (i >= dev->max_queues) {
			dev->kickfds[i] = -1;
			dev->callfds[i] = -1;
			continue;
		}

		/* May use invalid flag, but some backend uses kickfd and
		 * callfd as criteria to judge if dev is alive. so finally we
		 * use real event_fd.
		 */
		callfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (callfd < 0) {
			PMD_DRV_LOG(ERR, "callfd error, %s", strerror(errno));
			break;
		}
		kickfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (kickfd < 0) {
			PMD_DRV_LOG(ERR, "kickfd error, %s", strerror(errno));
			break;
		}
		dev->callfds[i] = callfd;
		dev->kickfds[i] = kickfd;
	}

	if (i < VIRTIO_MAX_VIRTQUEUES) {
		for (j = 0; j <= i; ++j) {
			close(dev->callfds[j]);
			close(dev->kickfds[j]);
		}

		return -1;
	}

	return 0;
}

static int
virtio_user_dev_setup(struct virtio_user_dev *dev)
{
	dev->vhostfd = -1;
	dev->vhostfds = NULL;
	dev->tapfds = NULL;

	dev->ops = &ops_user;

	if (dev->ops->setup(dev) < 0)
		return -1;

	if (virtio_user_dev_init_notify(dev) < 0)
		return -1;

	return 0;
}

/* Use below macro to filter features from vhost backend */
#define VIRTIO_USER_SUPPORTED_FEATURES			\
	(1ULL << VIRTIO_SCSI_F_INOUT		|	\
	 1ULL << VIRTIO_F_VERSION_1)

struct virtio_hw *
virtio_user_dev_init(char *path, int queues, int queue_size)
{
	struct virtio_hw *hw;
	struct virtio_user_dev *dev;
	uint64_t max_queues;

	hw = calloc(1, sizeof(*hw));
	dev = calloc(1, sizeof(struct virtio_user_dev));
	hw->virtio_user_dev = dev;

	virtio_hw_internal[hw->port_id].vtpci_ops = &virtio_user_ops;

	snprintf(dev->path, PATH_MAX, "%s", path);
	/* Account for control and event queue. */
	dev->max_queues = queues + 2;
	dev->queue_size = queue_size;

	if (virtio_user_dev_setup(dev) < 0) {
		PMD_INIT_LOG(ERR, "backend set up fails");
		free(hw);
		free(dev);
		return NULL;
	}

	if (dev->ops->send_request(dev, VHOST_USER_GET_QUEUE_NUM, &max_queues) < 0) {
		PMD_INIT_LOG(ERR, "get_queue_num fails: %s", strerror(errno));
		free(hw);
		free(dev);
		return NULL;
	}

	if (dev->max_queues > max_queues) {
		PMD_INIT_LOG(ERR, "%d queues requested but only %d supported", dev->max_queues, max_queues);
		free(hw);
		free(dev);
		return NULL;
	}

	if (dev->ops->send_request(dev, VHOST_USER_SET_OWNER, NULL) < 0) {
		PMD_INIT_LOG(ERR, "set_owner fails: %s", strerror(errno));
		free(hw);
		free(dev);
		return NULL;
	}

	if (dev->ops->send_request(dev, VHOST_USER_GET_FEATURES,
			    &dev->device_features) < 0) {
		PMD_INIT_LOG(ERR, "get_features failed: %s", strerror(errno));
		free(hw);
		free(dev);
		return NULL;
	}

	dev->device_features &= VIRTIO_USER_SUPPORTED_FEATURES;

	return hw;
}

void
virtio_user_dev_uninit(struct virtio_user_dev *dev)
{
	uint32_t i;

	virtio_user_stop_device(dev);

	for (i = 0; i < dev->max_queues; ++i) {
		close(dev->callfds[i]);
		close(dev->kickfds[i]);
	}

	close(dev->vhostfd);

	if (dev->vhostfds) {
		for (i = 0; i < dev->max_queues; ++i)
			close(dev->vhostfds[i]);
		free(dev->vhostfds);
		free(dev->tapfds);
	}
}
