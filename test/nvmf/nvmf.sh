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

timing_enter target

if [[ $SPDK_TEST_URING -eq 0 ]]; then
	run_test "nvmf_example" test/nvmf/target/nvmf_example.sh "${TEST_ARGS[@]}"
	run_test "nvmf_filesystem" test/nvmf/target/filesystem.sh "${TEST_ARGS[@]}"
	run_test "nvmf_discovery" test/nvmf/target/discovery.sh "${TEST_ARGS[@]}"
	run_test "nvmf_connect_disconnect" test/nvmf/target/connect_disconnect.sh "${TEST_ARGS[@]}"
	run_test "nvmf_multitarget" test/nvmf/target/multitarget.sh "${TEST_ARGS[@]}"
	run_test "nvmf_rpc" test/nvmf/target/rpc.sh "${TEST_ARGS[@]}"
	run_test "nvmf_invalid" test/nvmf/target/invalid.sh "${TEST_ARGS[@]}"
	run_test "nvmf_abort" test/nvmf/target/abort.sh "${TEST_ARGS[@]}"
	run_test "nvmf_ns_hotplug_stress" test/nvmf/target/ns_hotplug_stress.sh "${TEST_ARGS[@]}"
	run_test "nvmf_connect_stress" test/nvmf/target/connect_stress.sh "${TEST_ARGS[@]}"
	run_test "nvmf_fused_ordering" test/nvmf/target/fused_ordering.sh "${TEST_ARGS[@]}"
	run_test "nvmf_delete_subsystem" test/nvmf/target/delete_subsystem.sh "${TEST_ARGS[@]}"
	if [[ $SPDK_TEST_NVME_CLI -eq 1 ]]; then
		run_test "nvmf_nvme_cli" test/nvmf/target/nvme_cli.sh "${TEST_ARGS[@]}"
	fi
	if [[ $SPDK_TEST_VFIOUSER -eq 1 ]]; then
		run_test "nvmf_vfio_user" test/nvmf/target/nvmf_vfio_user.sh "${TEST_ARGS[@]}"
		run_test "nvmf_vfio_user_nvme_compliance" test/nvme/compliance/compliance.sh "${TEST_ARGS[@]}"
		run_test "nvmf_vfio_user_fuzz" test/nvmf/target/vfio_user_fuzz.sh "${TEST_ARGS[@]}"
	fi
fi

run_test "nvmf_host_management" test/nvmf/target/host_management.sh "${TEST_ARGS[@]}"
run_test "nvmf_lvol" test/nvmf/target/nvmf_lvol.sh "${TEST_ARGS[@]}"
run_test "nvmf_vhost" test/nvmf/target/nvmf_vhost.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdev_io_wait" test/nvmf/target/bdev_io_wait.sh "${TEST_ARGS[@]}"
run_test "nvmf_queue_depth" test/nvmf/target/queue_depth.sh "${TEST_ARGS[@]}"
run_test "nvmf_multipath" test/nvmf/target/multipath.sh "${TEST_ARGS[@]}"
run_test "nvmf_zcopy" test/nvmf/target/zcopy.sh "${TEST_ARGS[@]}"
run_test "nvmf_tls" test/nvmf/target/tls.sh "${TEST_ARGS[@]}"
run_test "nvmf_nmic" test/nvmf/target/nmic.sh "${TEST_ARGS[@]}"
run_test "nvmf_fio_target" test/nvmf/target/fio.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdevio" test/nvmf/target/bdevio.sh "${TEST_ARGS[@]}"

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "nvmf_fuzz" test/nvmf/target/fabrics_fuzz.sh "${TEST_ARGS[@]}"
	run_test "nvmf_multiconnection" test/nvmf/target/multiconnection.sh "${TEST_ARGS[@]}"
	run_test "nvmf_initiator_timeout" test/nvmf/target/initiator_timeout.sh "${TEST_ARGS[@]}"
fi

if [[ $NET_TYPE == phy ]]; then
	if [ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" ]; then
		gather_supported_nvmf_pci_devs
		TCP_INTERFACE_LIST=("${net_devs[@]}")
		if ((${#TCP_INTERFACE_LIST[@]} > 0)); then
			run_test "nvmf_perf_adq" test/nvmf/target/perf_adq.sh "${TEST_ARGS[@]}"
		fi
	fi
	run_test "nvmf_shutdown" test/nvmf/target/shutdown.sh "${TEST_ARGS[@]}"
	#TODO: disabled due to intermittent failures. Need to triage.
	# run_test "nvmf_srq_overwhelm" test/nvmf/target/srq_overwhelm.sh $TEST_ARGS
fi

timing_exit target

timing_enter host

if [[ $SPDK_TEST_URING -eq 0 ]]; then
	run_test "nvmf_multicontroller" test/nvmf/host/multicontroller.sh "${TEST_ARGS[@]}"
	run_test "nvmf_aer" test/nvmf/host/aer.sh "${TEST_ARGS[@]}"
	run_test "nvmf_async_init" test/nvmf/host/async_init.sh "${TEST_ARGS[@]}"
	run_test "dma" test/nvmf/host/dma.sh "${TEST_ARGS[@]}"
fi

run_test "nvmf_identify" test/nvmf/host/identify.sh "${TEST_ARGS[@]}"
run_test "nvmf_perf" test/nvmf/host/perf.sh "${TEST_ARGS[@]}"
run_test "nvmf_fio_host" test/nvmf/host/fio.sh "${TEST_ARGS[@]}"
run_test "nvmf_failover" test/nvmf/host/failover.sh "${TEST_ARGS[@]}"
run_test "nvmf_discovery" test/nvmf/host/discovery.sh "${TEST_ARGS[@]}"
# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh $TEST_ARGS

if [[ $SPDK_TEST_NVMF_MDNS -eq 1 && "$SPDK_TEST_NVMF_TRANSPORT" == "tcp" ]]; then
	# Skipping tests on RDMA because the rdma stack fails to configure the same IP for host and target.
	run_test "nvmf_mdns_discovery" test/nvmf/host/mdns_discovery.sh "${TEST_ARGS[@]}"
fi

if [[ $SPDK_TEST_USDT -eq 1 ]]; then
	run_test "nvmf_multipath" test/nvmf/host/multipath.sh "${TEST_ARGS[@]}"
	run_test "nvmf_timeout" test/nvmf/host/timeout.sh "${TEST_ARGS[@]}"
fi

if [[ $NET_TYPE == phy ]]; then
	# GitHub issue #1165
	run_test "nvmf_bdevperf" test/nvmf/host/bdevperf.sh "${TEST_ARGS[@]}"
	# GitHub issue #1043
	run_test "nvmf_target_disconnect" test/nvmf/host/target_disconnect.sh "${TEST_ARGS[@]}"
fi

timing_exit host

trap - SIGINT SIGTERM EXIT
