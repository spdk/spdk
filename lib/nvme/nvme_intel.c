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

/** \file
 * Intel specific data structures and functions.
 */
struct nvme_intel_quirk {
	struct pci_id	id;
	uint64_t	flags;
};

static const struct nvme_intel_quirk intel_p3x00[] = {
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3702}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3703}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3704}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3705}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x3709}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{SPDK_PCI_VID_INTEL, 0x0953, SPDK_PCI_VID_INTEL, 0x370a}, NVME_INTEL_QUIRK_READ_LATENCY | NVME_INTEL_QUIRK_WRITE_LATENCY	},
	{{0x0000, 0x0000, 0x0000, 0x0000}, 0												}
};

bool nvme_intel_has_quirk(struct pci_id *id, uint64_t quirk)
{
	const struct nvme_intel_quirk *intel_quirk = intel_p3x00;

	while (intel_quirk->id.vendor_id) {
		if (!memcmp(&intel_quirk->id, id, sizeof(*id)) && (intel_quirk->flags & quirk))
			return true;
		intel_quirk++;
	}
	return false;
}
