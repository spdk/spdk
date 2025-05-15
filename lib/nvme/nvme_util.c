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

int
spdk_nvme_trid_entry_parse(struct spdk_nvme_trid_entry *trid_entry, const char *str)
{
	struct spdk_nvme_transport_id *trid;
	char *ns, *hostnqn, *alt_traddr;
	size_t len;

	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, str) != 0) {
		SPDK_ERRLOG("Invalid transport ID format '%s'\n", str);
		return -EINVAL;
	}

	if ((ns = strcasestr(str, "ns:")) ||
	    (ns = strcasestr(str, "ns="))) {
		char nsid_str[6]; /* 5 digits maximum in an nsid */
		int nsid;

		ns += 3;

		len = strcspn(ns, " \t\n");
		if (len > 5) {
			SPDK_ERRLOG("NVMe namespace IDs must be 5 digits or less\n");
			return -EINVAL;
		}

		memcpy(nsid_str, ns, len);
		nsid_str[len] = '\0';

		nsid = spdk_strtol(nsid_str, 10);
		if (nsid <= 0 || nsid > 65535) {
			SPDK_ERRLOG("NVMe namespace IDs must be less than 65536 and greater than 0\n");
			return -EINVAL;
		}

		trid_entry->nsid = (uint16_t)nsid;
	}

	if ((hostnqn = strcasestr(str, "hostnqn:")) ||
	    (hostnqn = strcasestr(str, "hostnqn="))) {
		hostnqn += strlen("hostnqn:");
		len = strcspn(hostnqn, " \t\n");
		if (len > (sizeof(trid_entry->hostnqn) - 1)) {
			SPDK_ERRLOG("Host NQN is too long\n");
			return -EINVAL;
		}

		memcpy(trid_entry->hostnqn, hostnqn, len);
		trid_entry->hostnqn[len] = '\0';
	}

	trid_entry->failover_trid = trid_entry->trid;
	if ((alt_traddr = strcasestr(str, "alt_traddr:")) ||
	    (alt_traddr = strcasestr(str, "alt_traddr="))) {
		alt_traddr += strlen("alt_traddr:");
		len = strcspn(alt_traddr, " \t\n");
		if (len > SPDK_NVMF_TRADDR_MAX_LEN) {
			SPDK_ERRLOG("The failover traddr %s is too long.\n", alt_traddr);
			return -EINVAL;
		}

		snprintf(trid_entry->failover_trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1, "%s", alt_traddr);
	}

	return 0;
}

int
spdk_nvme_build_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr,
		     struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_transport_id *trid;
	int res, res2 = 0;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE: {
		struct spdk_pci_device *dev;

		res = snprintf(name, length, "PCIE (%s)", trid->traddr);

		dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (dev && res > 0) {
			struct spdk_pci_id pci_id;
			int _res;

			pci_id = spdk_pci_device_get_id(dev);
			_res = snprintf(name + res, length - res, " [%04x:%04x]", pci_id.vendor_id, pci_id.device_id);
			if (_res > 0) {
				res = res + _res;
			}
		}
		break;
	}
	case SPDK_NVME_TRANSPORT_RDMA:
		res = snprintf(name, length, "RDMA (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		res = snprintf(name, length, "TCP (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		res = snprintf(name, length, "VFIOUSER (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		res = snprintf(name, length, "CUSTOM (%s)", trid->traddr);
		break;
	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		res = -EINVAL;
	}

	if (res < 0) {
		return res;
	}

	if (ns) {
		res2 = snprintf(name + res, length - res, " NSID %u", spdk_nvme_ns_get_id(ns));
	}

	if (res2 < 0) {
		return res2;
	}

	return res + res2;
}
