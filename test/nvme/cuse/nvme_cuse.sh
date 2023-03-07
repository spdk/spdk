#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [[ $(uname) != "Linux" ]]; then
	echo "NVMe cuse tests only supported on Linux"
	exit 1
fi

modprobe cuse
run_test "nvme_cuse_app" $testdir/cuse
run_test "nvme_cuse_rpc" $testdir/nvme_cuse_rpc.sh
run_test "nvme_cli_cuse" $testdir/spdk_nvme_cli_cuse.sh
run_test "nvme_smartctl_cuse" $testdir/spdk_smartctl_cuse.sh
run_test "nvme_ns_manage_cuse" $testdir/nvme_ns_manage_cuse.sh
rmmod cuse

"$rootdir/scripts/setup.sh"
