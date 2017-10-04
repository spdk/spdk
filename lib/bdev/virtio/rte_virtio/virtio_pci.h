/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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

#ifndef _VIRTIO_PCI_H_
#define _VIRTIO_PCI_H_

#include <stdint.h>
#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>

#include <rte_pci.h>

#include "spdk/env.h"
#include "virtio_dev.h"

struct virtqueue;

/* Extra status define for readability */
#define VIRTIO_CONFIG_S_RESET 0

/* VirtIO PCI vendor/device ID. */
#define VIRTIO_PCI_VENDORID     0x1AF4
#define VIRTIO_PCI_DEVICEID_SCSI_MODERN 0x1004

struct virtio_pci_ops {
	void (*read_dev_cfg)(struct virtio_dev *hw, size_t offset,
			     void *dst, int len);
	void (*write_dev_cfg)(struct virtio_dev *hw, size_t offset,
			      const void *src, int len);
	uint8_t (*get_status)(struct virtio_dev *hw);
	void    (*set_status)(struct virtio_dev *hw, uint8_t status);

	uint64_t (*get_features)(struct virtio_dev *hw);
	void     (*set_features)(struct virtio_dev *hw, uint64_t features);

	uint8_t (*get_isr)(struct virtio_dev *hw);

	uint16_t (*set_config_irq)(struct virtio_dev *hw, uint16_t vec);

	/** Deinit and free virtio device */
	void (*free_vdev)(struct virtio_dev *vdev);
	uint16_t (*set_queue_irq)(struct virtio_dev *hw, struct virtqueue *vq,
			uint16_t vec);

	uint16_t (*get_queue_num)(struct virtio_dev *hw, uint16_t queue_id);
	int (*setup_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*del_queue)(struct virtio_dev *hw, struct virtqueue *vq);
	void (*notify_queue)(struct virtio_dev *hw, struct virtqueue *vq);
};

struct virtio_hw {
	struct virtio_dev vdev;
	uint8_t	    use_msix;
	uint32_t    notify_off_multiplier;
	uint8_t     *isr;
	uint16_t    *notify_base;

	struct {
		/** Mem-mapped resources from given PCI BAR */
		void        *vaddr;

		/** Length of the address space */
		uint32_t    len;
	} pci_bar[6];

	struct virtio_pci_common_cfg *common_cfg;
	struct spdk_pci_device *pci_dev;
	struct virtio_scsi_config *dev_cfg;
};

/*
 * While virtio_hw is stored in shared memory, this structure stores
 * some infos that may vary in the multiple process model locally.
 * For example, the vtpci_ops pointer.
 */
struct vtpci_internal {
	const struct virtio_pci_ops *vtpci_ops;
	struct rte_pci_ioport io;
};

#define VTPCI_OPS(dev)	(g_virtio_driver.internal[(dev)->port_id].vtpci_ops)
#define VTPCI_IO(dev)	(&g_virtio_driver.internal[(dev)->port_id].io)

struct virtio_driver {
	struct vtpci_internal internal[128];
	TAILQ_HEAD(, virtio_dev) init_ctrlrs;
	TAILQ_HEAD(, virtio_dev) attached_ctrlrs;
};

extern struct virtio_driver g_virtio_driver;

static inline int
vtpci_with_feature(struct virtio_dev *dev, uint64_t bit)
{
	return (dev->guest_features & (1ULL << bit)) != 0;
}

int vtpci_init(void);
void vtpci_reset(struct virtio_dev *);

void vtpci_reinit_complete(struct virtio_dev *);

uint8_t vtpci_get_status(struct virtio_dev *);
void vtpci_set_status(struct virtio_dev *, uint8_t);

uint64_t vtpci_negotiate_features(struct virtio_dev *, uint64_t);

void vtpci_write_dev_config(struct virtio_dev *, size_t, const void *, int);

void vtpci_read_dev_config(struct virtio_dev *, size_t, void *, int);

uint8_t vtpci_isr(struct virtio_dev *);

extern const struct virtio_pci_ops virtio_user_ops;

#endif /* _VIRTIO_PCI_H_ */
