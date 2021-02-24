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
#include "nvme.h"

static void
print_controller(const struct spdk_pci_addr *addr)
{
	char fmtaddr[32] = {};

	spdk_pci_addr_fmt(fmtaddr, sizeof(fmtaddr), addr);
	printf("Found NVMe controller at: %s\n", fmtaddr);
}

static void
attach_cb(void *cb_ctx, const struct spdk_pci_addr *addr,
	  struct nvme_ctrlr *ctrlr)
{
	(void)cb_ctx;

	print_controller(addr);
	nvme_detach(ctrlr);
}

int
main(int argc, const char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_pci_addr addr;
	struct nvme_ctrlr *ctrlr;
	int rc;

	spdk_env_opts_init(&opts);
	opts.name = "identify";

	if (spdk_env_init(&opts) != 0) {
		fprintf(stderr, "%s: unable to initialize SPDK env\n", argv[0]);
		return 1;
	}

	if (argc == 2) {
		rc = spdk_pci_addr_parse(&addr, argv[1]);
		if (rc != 0) {
			fprintf(stderr, "%s: failed to parse the address\n", argv[0]);
			return 1;
		}

		ctrlr = nvme_connect(&addr);
		if (!ctrlr) {
			fprintf(stderr, "%s: failed to connect to controller at %s\n",
				argv[0], argv[1]);
			return 1;
		}

		print_controller(&addr);
		nvme_detach(ctrlr);
	} else if (argc == 1) {
		rc = nvme_probe(attach_cb, NULL);
		if (rc != 0) {
			fprintf(stderr, "%s: nvme probe failed\n", argv[0]);
			return 1;
		}
	} else {
		fprintf(stderr, "Usage: %s [PCI_BDF_ADDRESS]\n", argv[0]);
		return 1;
	}

	return 0;
}
