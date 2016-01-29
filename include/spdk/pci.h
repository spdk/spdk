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

#ifndef SPDK_PCI_H
#define SPDK_PCI_H

#define spdk_pci_device_get_domain(dev)	(dev->domain)
#define spdk_pci_device_get_bus(dev)	(dev->bus)
#define spdk_pci_device_get_dev(pdev)	(pdev->dev)
#define spdk_pci_device_get_func(dev)	(dev->func)
#define spdk_pci_device_get_vendor_id(dev) (dev->vendor_id)
#define spdk_pci_device_get_device_id(dev) (dev->device_id)
#define spdk_pci_device_get_subvendor_id(dev) (dev->subvendor_id)
#define spdk_pci_device_get_subdevice_id(dev) (dev->subdevice_id)

#define PCI_CFG_SIZE		256
#define PCI_EXT_CAP_ID_SN	0x03
#define PCI_UIO_DRIVER		"uio_pci_generic"

int pci_device_get_serial_number(struct pci_device *dev, char *sn, int len);
int pci_device_has_non_uio_driver(struct pci_device *dev);
int pci_device_unbind_kernel_driver(struct pci_device *dev);
int pci_device_bind_uio_driver(struct pci_device *dev, char *driver_name);
int pci_device_switch_to_uio_driver(struct pci_device *pci_dev);
int pci_device_claim(struct pci_device *dev);

#endif
