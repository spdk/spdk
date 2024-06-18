/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
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
print_pci_dev(void *ctx, struct spdk_pci_device *dev)
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
	int op, rc = 0;
	struct spdk_env_opts opts;

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
		rc = 1;
		goto exit;
	}

	printf("\nList of available PCI devices:\n");
	spdk_pci_for_each_device(NULL, print_pci_dev);

exit:
	spdk_vmd_fini();
	spdk_env_fini();

	return rc;
}
