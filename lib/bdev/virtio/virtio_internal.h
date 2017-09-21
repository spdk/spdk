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

#ifndef SPDK_BDEV_VIRTIO_INTERNAL_H
#define SPDK_BDEV_VIRTIO_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/env.h"

#include <sys/queue.h>

struct virtio_dev {
	struct virtqueue		**vqs;
	uint16_t			started;
	uint32_t			max_queues;
	uint8_t				port_id;
	uint64_t			req_guest_features;
	uint64_t			guest_features;
	int				is_hw;
	uint8_t				modern;
};

struct virtio_hw {
	struct virtio_dev		vdev;
	uint8_t				use_msix;
	uint32_t			notify_off_multiplier;
	uint8_t				*isr;
	uint16_t			*notify_base;
	struct virtio_pci_common_cfg	*common_cfg;
	struct spdk_pci_device		*pci_dev;
	struct virtio_scsi_config	*dev_cfg;

	TAILQ_ENTRY(virtio_hw)		tailq;
};

struct virtio_driver {
	TAILQ_HEAD(, virtio_hw)		vdevs;
};

extern struct virtio_driver g_spdk_virtio_driver;

#endif /* SPDK_BDEV_VIRTIO_INTERNAL_H */
