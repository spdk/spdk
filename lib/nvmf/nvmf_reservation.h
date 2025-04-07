/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __NVMF_RESERVATION_H__
#define __NVMF_RESERVATION_H__

struct _nvmf_ns_registrant {
	uint64_t		rkey;
	char			*host_uuid;
};

struct _nvmf_ns_registrants {
	size_t				num_regs;
	struct _nvmf_ns_registrant	reg[SPDK_NVMF_MAX_NUM_REGISTRANTS];
};

struct _nvmf_ns_reservation {
	uint64_t				version;
	uint64_t				epoch;
	bool					ptpl_activated;
	enum spdk_nvme_reservation_type		rtype;
	uint64_t				crkey;
	char					*bdev_uuid;
	char					*holder_uuid;
	struct _nvmf_ns_registrants		regs;
};


void spdk_try_rbd_reservation_ops_set(struct spdk_bdev *bdev);

void spdk_nvmf_set_custom_ns_reservation_ops(const struct spdk_nvmf_ns_reservation_ops *ops);

uint32_t nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns);

int nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);

#endif
