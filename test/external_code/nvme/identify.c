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
print_ascii_string(const void *buf, size_t size)
{
	const uint8_t *str = buf;

	/* Trim trailing spaces */
	while (size > 0 && str[size - 1] == ' ') {
		size--;
	}

	while (size--) {
		if (*str >= 0x20 && *str <= 0x7E) {
			printf("%c", *str);
		} else {
			printf(".");
		}
		str++;
	}
}

static void
print_controller(struct nvme_ctrlr *ctrlr, const struct spdk_pci_addr *addr)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	char fmtaddr[32] = {};

	cdata = nvme_ctrlr_get_data(ctrlr);
	spdk_pci_addr_fmt(fmtaddr, sizeof(fmtaddr), addr);

	printf("=====================================================\n");
	printf("NVMe Controller at %s\n", fmtaddr);
	printf("=====================================================\n");
	printf("Vendor ID:                             %04x\n", cdata->vid);
	printf("Subsystem Vendor ID:                   %04x\n", cdata->ssvid);
	printf("Serial Number:                         ");
	print_ascii_string(cdata->sn, sizeof(cdata->sn));
	printf("\n");
	printf("Model Number:                          ");
	print_ascii_string(cdata->mn, sizeof(cdata->mn));
	printf("\n");
	printf("Firmware Version:                      ");
	print_ascii_string(cdata->fr, sizeof(cdata->fr));
	printf("\n");
	printf("Recommended Arb Burst:                 %d\n", cdata->rab);
	printf("IEEE OUI Identifier:                   %02x %02x %02x\n",
	       cdata->ieee[0], cdata->ieee[1], cdata->ieee[2]);
	printf("Multi-path I/O\n");
	printf("  May have multiple subsystem ports:   %s\n", cdata->cmic.multi_port ? "Yes" : "No");
	printf("  May have multiple controllers:       %s\n", cdata->cmic.multi_ctrlr ? "Yes" : "No");
	printf("  Associated with SR-IOV VF:           %s\n", cdata->cmic.sr_iov ? "Yes" : "No");
	printf("Max Number of Namespaces:              %d\n", cdata->nn);
	if (cdata->ver.raw != 0) {
		printf("NVMe Specification Version (Identify): %u.%u",
		       cdata->ver.bits.mjr, cdata->ver.bits.mnr);
		if (cdata->ver.bits.ter) {
			printf(".%u", cdata->ver.bits.ter);
		}
		printf("\n");
	}
	printf("Optional Asynchronous Events Supported\n");
	printf("  Namespace Attribute Notices:         %s\n",
	       cdata->oaes.ns_attribute_notices ? "Supported" : "Not Supported");
	printf("  Firmware Activation Notices:         %s\n",
	       cdata->oaes.fw_activation_notices ? "Supported" : "Not Supported");
	printf("128-bit Host Identifier:               %s\n",
	       cdata->ctratt.host_id_exhid_supported ? "Supported" : "Not Supported");
}

static void
attach_cb(void *cb_ctx, const struct spdk_pci_addr *addr,
	  struct nvme_ctrlr *ctrlr)
{
	(void)cb_ctx;

	print_controller(ctrlr, addr);
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

		print_controller(ctrlr, &addr);
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
