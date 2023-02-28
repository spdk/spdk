#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y = nvme.c nvme_ctrlr.c nvme_ctrlr_cmd.c nvme_ctrlr_ocssd_cmd.c nvme_ns.c nvme_ns_cmd.c nvme_ns_ocssd_cmd.c nvme_pcie.c nvme_poll_group.c nvme_qpair.c \
	 nvme_quirks.c nvme_tcp.c nvme_transport.c nvme_io_msg.c nvme_pcie_common.c nvme_fabric.c nvme_opal.c \

DIRS-$(CONFIG_RDMA) += nvme_rdma.c
DIRS-$(CONFIG_NVME_CUSE) += nvme_cuse.c

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
