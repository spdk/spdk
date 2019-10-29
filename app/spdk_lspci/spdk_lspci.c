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

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/vmd.h"

static void
usage(void)
{
	printf("Usage: spdk_lspci\n");
	printf("Print available SPDK PCI devices supported by NVMe driver.\n");
}

static int
pci_enum_cb(void *ctx, struct spdk_pci_device *dev)
{
	return 0;
}

static void
print_pci_dev(struct spdk_pci_device *dev)
{
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(dev);
	char addr[32] = { 0 };

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr);

	printf("%s (%x %x)", addr,
	       spdk_pci_device_get_vendor_id(dev),
	       spdk_pci_device_get_device_id(dev));

	if (strcmp(spdk_pci_device_get_type(dev), "vmd") == 0) {
		printf(" (NVMe disk behind VMD) ");
	}

	if (dev->internal.driver == spdk_pci_vmd_get_driver()) {
		printf(" (VMD) ");
	}

	printf("\n");
}

int
main(int argc, char **argv)
{
	int op;
	struct spdk_env_opts opts;
	struct spdk_pci_device *dev;

	while ((op = getopt(argc, argv, "h")) != -1) {
		switch (op) {
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 1;
		}
	}

	spdk_env_opts_init(&opts);
	opts.name = "spdk_lspci";

	if (spdk_env_init(&opts) < 0) {
		printf("Unable to initialize SPDK env\n");
		return 1;
	}

	if (spdk_vmd_init()) {
		printf("Failed to initialize VMD. Some NVMe devices can be unavailable.\n");
	}

	if (spdk_pci_enumerate(spdk_pci_nvme_get_driver(), pci_enum_cb, NULL)) {
		printf("Unable to enumerate PCI nvme driver\n");
		return 1;
	}

	dev = spdk_pci_get_first_device();
	if (!dev) {
		printf("\nLack of PCI devices available for SPDK!\n");
	}

	printf("\nList of available PCI devices:\n");
	while (dev) {
		print_pci_dev(dev);
		dev = spdk_pci_get_next_device(dev);
	}

	spdk_vmd_fini();

	return 0;
}
