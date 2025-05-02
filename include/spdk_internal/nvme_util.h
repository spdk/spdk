/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2025 Nutanix Inc.
 */

#ifndef SPDK_INTERNAL_NVME_UTIL_H
#define SPDK_INTERNAL_NVME_UTIL_H

#include "spdk/nvme.h"

enum spdk_nvme_trid_usage_opt {
	SPDK_NVME_TRID_USAGE_OPT_MANDATORY = 1 << 1,
	SPDK_NVME_TRID_USAGE_OPT_LONGOPT  = 1 << 2,
	SPDK_NVME_TRID_USAGE_OPT_NO_PCIE = 1 << 3,
	SPDK_NVME_TRID_USAGE_OPT_NO_FABRIC = 1 << 4,
	SPDK_NVME_TRID_USAGE_OPT_MULTI = 1 << 5,
	SPDK_NVME_TRID_USAGE_OPT_NS = 1 << 6,
	SPDK_NVME_TRID_USAGE_OPT_HOSTNQN = 1 << 7,
	SPDK_NVME_TRID_USAGE_OPT_ALT_TRADDR = 1 << 8,
};

struct spdk_nvme_trid_entry {
	struct spdk_nvme_transport_id trid;
	uint16_t nsid;
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	struct spdk_nvme_transport_id	failover_trid;
};

/**
 * Prints Transport ID format and its description.
 *
 * \param f File to hold output information.
 * \param opts Include options specified in the mask; \ref enum spdk_nvme_trid_usage_opt to decode.
 */
void spdk_nvme_transport_id_usage(FILE *f, uint32_t opts);

/**
 * Parses the string representation of a transport ID with extra key:value pairs.
 *
 * See \ref spdk_nvme_transport_id_parse for more details and base key:value pairs.
 *
 * Key          | Value
 * ------------ | -----
 * ns           | NVMe namespace ID (all active namespaces are used by default)
 * hostnqn      | Host NQN
 * alt_traddr   | Alternative Transport address for failover
 *
 * \param str Input string representation of a transport ID to parse.
 * \param trid_entry Output transport ID structure (must be allocated and initialized by caller).
 *
 * \return 0 if parsing was successful and trid is filled out, or negated errno
 * values on failure.
 */
int spdk_nvme_trid_entry_parse(struct spdk_nvme_trid_entry *trid_entry, const char *str);

/**
 * Builds NVMe name.
 *
 * \param name Pointer to a character string where NVMe name is meant to be built.
 * \param length Length of the given character string, plus the null terminator.
 * \param ctrlr NVMe controller.
 * \param ns Namespace (optional)
 *
 * \return Number of characters not including the terminating null character or negative on failure.
 */
int spdk_nvme_build_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr,
			 struct spdk_nvme_ns *ns);

#endif /* SPDK_INTERNAL_NVME_UTIL_H */
