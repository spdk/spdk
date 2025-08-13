#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

trap "exit 1" SIGINT SIGTERM EXIT

TEST_ARGS=("$@")

if [[ $SPDK_TEST_URING -eq 0 ]]; then
	run_test "nvmf_multicontroller" $rootdir/test/nvmf/host/multicontroller.sh "${TEST_ARGS[@]}"
	run_test "nvmf_aer" $rootdir/test/nvmf/host/aer.sh "${TEST_ARGS[@]}"
	run_test "nvmf_async_init" $rootdir/test/nvmf/host/async_init.sh "${TEST_ARGS[@]}"
	run_test "dma" $rootdir/test/nvmf/host/dma.sh "${TEST_ARGS[@]}"
fi

run_test "nvmf_identify" $rootdir/test/nvmf/host/identify.sh "${TEST_ARGS[@]}"
run_test "nvmf_perf" $rootdir/test/nvmf/host/perf.sh "${TEST_ARGS[@]}"
run_test "nvmf_fio_host" $rootdir/test/nvmf/host/fio.sh "${TEST_ARGS[@]}"
run_test "nvmf_failover" $rootdir/test/nvmf/host/failover.sh "${TEST_ARGS[@]}"
run_test "nvmf_host_discovery" $rootdir/test/nvmf/host/discovery.sh "${TEST_ARGS[@]}"
run_test "nvmf_host_multipath_status" $rootdir/test/nvmf/host/multipath_status.sh "${TEST_ARGS[@]}"
run_test "nvmf_discovery_remove_ifc" $rootdir/test/nvmf/host/discovery_remove_ifc.sh "${TEST_ARGS[@]}"

if [[ "$SPDK_TEST_SKIP_NVMF_KERNEL_TESTS" -eq 0 ]]; then
	run_test "nvmf_identify_kernel_target" "$rootdir/test/nvmf/host/identify_kernel_nvmf.sh" "${TEST_ARGS[@]}"
	run_test "nvmf_auth_host" "$rootdir/test/nvmf/host/auth.sh" "${TEST_ARGS[@]}"
fi

if [[ "$SPDK_TEST_NVMF_TRANSPORT" == "tcp" ]]; then
	run_test "nvmf_digest" "$rootdir/test/nvmf/host/digest.sh" "${TEST_ARGS[@]}"
fi

if [[ $SPDK_TEST_NVMF_MDNS -eq 1 && "$SPDK_TEST_NVMF_TRANSPORT" == "tcp" ]]; then
	# Skipping tests on RDMA because the rdma stack fails to configure the same IP for host and target.
	run_test "nvmf_mdns_discovery" $rootdir/test/nvmf/host/mdns_discovery.sh "${TEST_ARGS[@]}"
fi

if [[ $SPDK_TEST_USDT -eq 1 ]]; then
	run_test "nvmf_host_multipath" $rootdir/test/nvmf/host/multipath.sh "${TEST_ARGS[@]}"
	run_test "nvmf_timeout" $rootdir/test/nvmf/host/timeout.sh "${TEST_ARGS[@]}"
fi

run_test "nvmf_bdevperf" $rootdir/test/nvmf/host/bdevperf.sh "${TEST_ARGS[@]}"
run_test "nvmf_target_disconnect" $rootdir/test/nvmf/host/target_disconnect.sh "${TEST_ARGS[@]}"
run_test "nvmf_host_ns_hotplug_stress" $rootdir/test/nvmf/host/ns_hotplug_stress.sh "${TEST_ARGS[@]}"

trap - SIGINT SIGTERM EXIT
