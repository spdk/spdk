/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2025 Nutanix Inc.
 */

#include "spdk_internal/nvme_util.h"
#include "spdk/log.h"
#include "spdk/string.h"

void
spdk_nvme_transport_id_usage(FILE *f, uint32_t opts)
{
	bool mandatory = opts & SPDK_NVME_TRID_USAGE_OPT_MANDATORY;
	bool pcie = !(opts & SPDK_NVME_TRID_USAGE_OPT_NO_PCIE);
	bool fabric = !(opts & SPDK_NVME_TRID_USAGE_OPT_NO_FABRIC);
	bool both = pcie && fabric;
	const char *pcie_addr = pcie ? "0000:04:00.0" : "";
	const char *fabric_addr = fabric ? "192.168.100.8" : "";
	const char * or = both ? " or " : "";

	fprintf(f, "\t%s-r%s <fmt> Transport ID for %s%s%s%s\n", mandatory ? "" : "[",
		opts & SPDK_NVME_TRID_USAGE_OPT_LONGOPT ? ", --transport" : "", pcie ? "local PCIe NVMe" : "", or
		, fabric ? "NVMeoF" : "", mandatory ? "" : "]");
	fprintf(f, "\t\tFormat: 'key:value [key:value] ...'\n");
	fprintf(f, "\t\tKeys:\n");
	fprintf(f, "\t\t trtype      Transport type (e.g. PCIe, RDMA)\n");
	if (fabric) {
		fprintf(f, "\t\t adrfam      Address family (e.g. IPv4, IPv6)\n");
	}

	fprintf(f, "\t\t traddr      Transport address (e.g. %s%s%s)\n", pcie_addr, or, fabric_addr);

	if (fabric) {
		fprintf(f, "\t\t trsvcid     Transport service identifier (e.g. 4420)\n");
		fprintf(f, "\t\t subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	}

	if (opts & SPDK_NVME_TRID_USAGE_OPT_NS) {
		fprintf(f, "\t\t %-11s NVMe namespace ID (all active namespaces are used by default)\n", "ns");
	}

	if (fabric && opts & SPDK_NVME_TRID_USAGE_OPT_HOSTNQN) {
		fprintf(f, "\t\t %-11s Host NQN\n", "hostnqn");
	}

	if (fabric && opts & SPDK_NVME_TRID_USAGE_OPT_ALT_TRADDR) {
		fprintf(f, "\t\t %-11s Alternative Transport address for failover (optional)\n", "alt_traddr");
	}

	fprintf(f, "\t\tExamples:\n");
	if (both || pcie) {
		fprintf(f, "\t\t -r 'trtype:PCIe traddr:%s'\n", pcie_addr);
	}

	if (both || fabric) {
		fprintf(f, "\t\t -r 'trtype:RDMA adrfam:IPv4 traddr:%s trsvcid:4420'\n", fabric_addr);
	}

	if (opts & SPDK_NVME_TRID_USAGE_OPT_MULTI) {
		fprintf(f, "\t\tNote: can be specified multiple times to test multiple disks/targets.\n");
	}
}
