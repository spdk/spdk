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

#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/env.h"
#include "spdk/nvme_intel.h"
#include "spdk/nvmf_spec.h"
#include "spdk/pci_ids.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/vmd.h"


static struct spdk_nvme_transport_id g_trid, g_vmd_trid;

static int parse_args(int argc, char **argv)
{
	int op;

	g_trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	g_vmd_trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:i:p:v:r:xHL:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing nvme-transport address\n");
				return 1;
			}
			break;

		default:
			return 1;
		}
	}

	return 0;
}

/*
 *******************************************************************************
 * @brief - Typical usage:
 *   1: user runs lspci to list vmd pci device(domain:bus:dev:func)
 *   2: user runs lspci-vmd to list enumerate the devices behind the targeted VMD
 *          -(no arg to lspci-vmd lists enumerates all vmd, or BDF targets input vmd.
 *   3:user then targets one of the nvme devices listed by lspci-vmd for i/o, etc
 *      in this example, the input BDF to 'init' will list the identify info for that
 *      nvme device behind vmd.
 *
 *  In this example, user enters VMD BDF and nvme BDF after lspci and lspci-vmd
 *    to display the nvme identify info behind the targetewd vmd.
 *
 * argument format: init -v(or --vmd) <vmd-domain:vmd-bus:vmd-dev:vmd-func> -r <nvme-domain:nvme_bus:nvme_dev:nvme_func>
 *  all arguments are specified as hexadecimal format 0xdddd.
 *
 * *****************************************************************************
 */
int main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_pci_addr addr = {0x0000, 0x0, 0x00, 0x0};
	struct spdk_pci_addr *probe_addr;

	int rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "lsvmd";
	opts.mem_channel = 1;

	if (g_trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		opts.no_pci = true;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("%s: Initializing VMD Controllers\n", __func__);

	probe_addr = &addr;
	if (spdk_pci_addr_parse(&addr, g_trid.traddr) != 0) {
		printf(" Invalid nvme BDF address requested: %s. Probing ALL VMD devices\n", g_trid.traddr);
		probe_addr = NULL;
	}

	/*
	 * pass the input VMD domain:BDF to vmd probe.
	 */
	rc = spdk_vmd_probe(probe_addr);

	return 0;
}
