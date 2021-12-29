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

int main(int argc, char **argv)
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
