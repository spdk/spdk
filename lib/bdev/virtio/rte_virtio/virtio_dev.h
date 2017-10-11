/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#ifndef _VIRTIO_DEV_H_
#define _VIRTIO_DEV_H_

#include <stdint.h>
#include <sys/uio.h>
#include <sys/queue.h>

/* This is a work-around for fio-plugin bug, where each
 * fio job thread returns local lcore id = -1
 */
#define SPDK_VIRTIO_QUEUE_LCORE_ID_UNUSED (UINT32_MAX - 1)

struct virtio_dev {
	struct virtqueue **vqs;
	uint16_t	started;

	/** Max number of queues the host supports. */
	uint16_t	max_queues;

	/** Device index. */
	uint32_t	id;

	/** Common device & guest features. */
	uint64_t	negotiated_features;

	int		is_hw;

	/** Modern/legacy virtio device flag. */
	uint8_t		modern;

	/** Mutex for asynchronous virtqueue-changing operations. */
	pthread_mutex_t	mutex;

	TAILQ_ENTRY(virtio_dev) tailq;
};

struct virtio_req {
	struct iovec	*iov;
	struct iovec	iov_req;
	struct iovec	iov_resp;
	uint32_t	iovcnt;
	int		is_write;
	uint32_t	data_transferred;
};

/* Features desired/implemented by this driver. */
#define VIRTIO_SCSI_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_SCSI_F_INOUT		|	\
	 1ULL << VIRTIO_F_VERSION_1)

uint16_t virtio_recv_pkts(struct virtqueue *vq, struct virtio_req **reqs,
		uint16_t nb_pkts);

uint16_t virtio_xmit_pkts(struct virtqueue *vq, struct virtio_req *req);

int virtio_dev_init(struct virtio_dev *hw, uint64_t req_features);
void virtio_dev_free(struct virtio_dev *dev);
int virtio_dev_start(struct virtio_dev *hw);

/**
 * Look for unused queue and bind it to the current CPU core.  This will
 * scan the queues in range from *start_index* (inclusive) up to
 * vdev->max_queues (exclusive).
 *
 * This function is thread-safe.
 *
 * \param vdev vhost device
 * \param start_index virtqueue index to start looking from
 * \return acquired queue or NULL in case no unused queue in given range
 * has been found
 */
struct virtqueue *virtio_dev_acquire_queue(struct virtio_dev *vdev, uint16_t start_index);

/**
 * Look for a virtqueue acquired by the current lcore.  If multiple queues
 * are found, the one with the lowest index will be returned.
 *
 * \param vdev vhost device
 * \return virtqueue or NULL
 */
struct virtqueue *virtio_dev_get_acquired_queue(struct virtio_dev *vdev);

/**
 * Release previously acquired queue.
 *
 * This function must be called from the thread that acquired the queue.
 *
 * \param vdev vhost device
 * \param vq virtqueue to release
 */
void virtio_dev_release_queue(struct virtio_dev *vdev, struct virtqueue *vq);

#endif /* _VIRTIO_DEV_H_ */
