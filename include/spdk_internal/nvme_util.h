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

/**
 * Prints Transport ID format and its description.
 *
 * \param f File to hold output information.
 * \param opts Include options specified in the mask; \ref enum spdk_nvme_trid_usage_opt to decode.
 */
void spdk_nvme_transport_id_usage(FILE *f, uint32_t opts);

#endif /* SPDK_INTERNAL_NVME_UTIL_H */
