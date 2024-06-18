/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/vmd.h"

struct spdk_pci_addr g_probe_addr;

static int
parse_args(int argc, char **argv)
{
	int op;

	while ((op = getopt(argc, argv, "r:d")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_pci_addr_parse(&g_probe_addr, optarg)) {
				SPDK_ERRLOG("Error parsing PCI address\n");
				return 1;
			}

			break;

		case 'd':
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			spdk_log_set_flag("vmd");
			break;

		default:
			return 1;
		}
	}

	return 0;
}

static void
print_device(void *ctx, struct spdk_pci_device *pci_device)
{
	char addr_buf[128];
	int rc;

	if (strcmp(spdk_pci_device_get_type(pci_device), "vmd") == 0) {
		rc = spdk_pci_addr_fmt(addr_buf, sizeof(addr_buf), &pci_device->addr);
		if (rc != 0) {
			fprintf(stderr, "Failed to format VMD's PCI address\n");
			return;
		}

		printf("%s\n", addr_buf);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int rc;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "lsvmd";

	if (spdk_env_init(&opts) < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		return 1;
	}

	rc = spdk_vmd_init();
	if (rc) {
		SPDK_ERRLOG("No VMD Controllers found\n");
	}

	spdk_pci_for_each_device(NULL, print_device);

	spdk_vmd_fini();

	spdk_env_fini();
	return rc;
}
