#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit

trap 'nvmftestfini || :; clean_kernel_target' EXIT

target_ip=$(get_main_ns_ip)
configure_kernel_target "$NVME_SUBNQN" "$target_ip"

"$SPDK_BIN_DIR/spdk_nvme_identify" -r "\
	trtype:$TEST_TRANSPORT \
	adrfam:IPv4 \
	traddr:$target_ip
	trsvcid:$NVMF_PORT \
	subnqn:nqn.2014-08.org.nvmexpress.discovery" \
	"${NO_HUGE[@]}"
$SPDK_BIN_DIR/spdk_nvme_identify -r "\
	trtype:$TEST_TRANSPORT \
	adrfam:IPv4 \
	traddr:$target_ip \
	trsvcid:$NVMF_PORT \
	subnqn:$NVME_SUBNQN" \
	"${NO_HUGE[@]}"
