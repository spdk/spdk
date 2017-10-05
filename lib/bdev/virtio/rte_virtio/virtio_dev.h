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

struct virtio_dev {
	struct virtqueue **vqs;
	uint16_t	started;

	/** Max number of queues the host supports. */
	uint16_t	max_queues;

	/* Device index. */
	uint32_t	id;

	/* Common device & guest features. */
	uint64_t	negotiated_features;

	int		is_hw;

	/** Modern/legacy virtio device flag. */
	uint8_t		modern;

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

/**
 *
 * \param vq
 * \param req
 * \return
 * 0 on succes or negative error code.
 * -EAGAIN - no room in queue.
 * -EIO - given VQ is not started.
 */
int virtio_xmit_pkts(struct virtqueue *vq, struct virtio_req *req);

int virtio_dev_init(struct virtio_dev *hw, uint64_t req_features);
void virtio_dev_free(struct virtio_dev *dev);
int virtio_dev_start(struct virtio_dev *hw);
struct virtio_dev *get_pci_virtio_hw(void);

void virtio_interrupt_handler(void *param);

#endif /* _VIRTIO_DEV_H_ */
