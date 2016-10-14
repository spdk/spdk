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

#include "nvme_internal.h"


struct nvme_quirk {
	struct pci_id	id;
	uint64_t	flags;
};

static const struct nvme_quirk nvme_quirks[] = {
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3702}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3703}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3704}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3705}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3709}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x370a}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_MEMBLAZE, 0x0540, SPDK_PCI_ANY_ID, SPDK_PCI_ANY_ID}, NVME_QUIRK_DELAY_BEFORE_CHK_RDY },
	{{0x0000, 0x0000, 0x0000, 0x0000}, 0												}
};

/* Compare each field. SPDK_PCI_ANY_ID matches everything */
static bool pci_id_match(const struct pci_id *s1, const struct pci_id *s2)
{
	if (((s1->vendor_id == SPDK_PCI_ANY_ID) || (s2->vendor_id == SPDK_PCI_ANY_ID) || 
			(s1->vendor_id == s2->vendor_id)) &&
	    ((s1->dev_id == SPDK_PCI_ANY_ID) || (s2->dev_id == SPDK_PCI_ANY_ID) ||
			(s1->dev_id == s2->dev_id)) &&
		((s1->sub_vendor_id == SPDK_PCI_ANY_ID) || (s2->sub_vendor_id == SPDK_PCI_ANY_ID) ||
			(s1->sub_vendor_id == s2->sub_vendor_id)) &&
		((s1->sub_dev_id == SPDK_PCI_ANY_ID) || (s2->sub_dev_id == SPDK_PCI_ANY_ID) ||
			(s1->sub_dev_id == s2->sub_dev_id))) {
		return true;
	}
	return false;
}

uint64_t nvme_get_ctrlr_quirk(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct nvme_quirk *quirk = nvme_quirks;
	struct pci_id pci_id;

	ctrlr->transport->ctrlr_get_pci_id(ctrlr, &pci_id);

	while (quirk->id.vendor_id) {
		if (pci_id_match(&quirk->id, &pci_id))
			return quirk->flags;
		quirk++;
	}
	return 0;
}
