#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

trap "exit 1" SIGINT SIGTERM EXIT

TEST_ARGS=("$@")

if [[ $SPDK_TEST_URING -eq 0 ]]; then
	run_test "nvmf_abort" $rootdir/test/nvmf/target/abort.sh "${TEST_ARGS[@]}"
	run_test "nvmf_ns_hotplug_stress" $rootdir/test/nvmf/target/ns_hotplug_stress.sh "${TEST_ARGS[@]}"
	run_test "nvmf_delete_subsystem" $rootdir/test/nvmf/target/delete_subsystem.sh "${TEST_ARGS[@]}"
fi

run_test "nvmf_host_management" $rootdir/test/nvmf/target/host_management.sh "${TEST_ARGS[@]}"
run_test "nvmf_lvol" $rootdir/test/nvmf/target/nvmf_lvol.sh "${TEST_ARGS[@]}"
run_test "nvmf_lvs_grow" $rootdir/test/nvmf/target/nvmf_lvs_grow.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdev_io_wait" $rootdir/test/nvmf/target/bdev_io_wait.sh "${TEST_ARGS[@]}"
run_test "nvmf_queue_depth" $rootdir/test/nvmf/target/queue_depth.sh "${TEST_ARGS[@]}"
run_test "nvmf_zcopy" $rootdir/test/nvmf/target/zcopy.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdevio" $rootdir/test/nvmf/target/bdevio.sh "${TEST_ARGS[@]}"

if [[ "$SPDK_TEST_SKIP_NVMF_KERNEL_TESTS" -eq 0 ]]; then
	run_test "nvmf_target_multipath" $rootdir/test/nvmf/target/multipath.sh "${TEST_ARGS[@]}"
	run_test "nvmf_nmic" $rootdir/test/nvmf/target/nmic.sh "${TEST_ARGS[@]}"
	run_test "nvmf_fio_target" $rootdir/test/nvmf/target/fio.sh "${TEST_ARGS[@]}"
fi

trap - SIGINT SIGTERM EXIT
