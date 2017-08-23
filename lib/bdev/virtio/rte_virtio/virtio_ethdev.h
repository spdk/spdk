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

#ifndef _VIRTIO_ETHDEV_H_
#define _VIRTIO_ETHDEV_H_

#include <stdint.h>
#include <sys/uio.h>

#include "virtio_pci.h"

#define VIRTIO_MAX_RX_QUEUES 128U
#define VIRTIO_MAX_TX_QUEUES 128U
#define VIRTIO_MIN_RX_BUFSIZE 64

struct virtio_req {
	struct iovec	*iov;
	struct iovec	iov_req;
	struct iovec	iov_resp;
	uint32_t	iovcnt;
	int		is_write;
	uint32_t	data_transferred;
};

/* Features desired/implemented by this driver. */
#define VIRTIO_PMD_DEFAULT_GUEST_FEATURES	\
	(1u << VIRTIO_SCSI_F_INOUT	  |	\
	 1ULL << VIRTIO_F_VERSION_1       |	\
	 1ULL << VIRTIO_F_IOMMU_PLATFORM)

#define VIRTIO_PMD_SUPPORTED_GUEST_FEATURES	\
	(VIRTIO_PMD_DEFAULT_GUEST_FEATURES)

/*
 * RX/TX function prototypes
 */

int  virtio_dev_tx_queue_setup(struct virtio_hw *hw, uint16_t tx_queue_id,
		uint16_t nb_tx_desc, unsigned int socket_id);

uint16_t virtio_recv_pkts(struct virtqueue *vq, struct virtio_req **reqs,
		uint16_t nb_pkts);

uint16_t virtio_xmit_pkts(struct virtqueue *vq, struct virtio_req *req);

int eth_virtio_dev_init(struct virtio_hw *hw, int num_queues);
int virtio_dev_start(struct virtio_hw *hw);
struct virtio_hw *get_pci_virtio_hw(void);

void virtio_interrupt_handler(void *param);

#endif /* _VIRTIO_ETHDEV_H_ */
