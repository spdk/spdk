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
	run_test "nvmf_example" $rootdir/test/nvmf/target/nvmf_example.sh "${TEST_ARGS[@]}"
	run_test "nvmf_filesystem" $rootdir/test/nvmf/target/filesystem.sh "${TEST_ARGS[@]}"
	run_test "nvmf_target_discovery" $rootdir/test/nvmf/target/discovery.sh "${TEST_ARGS[@]}"
	run_test "nvmf_referrals" $rootdir/test/nvmf/target/referrals.sh "${TEST_ARGS[@]}"
	run_test "nvmf_connect_disconnect" $rootdir/test/nvmf/target/connect_disconnect.sh "${TEST_ARGS[@]}"
	run_test "nvmf_multitarget" $rootdir/test/nvmf/target/multitarget.sh "${TEST_ARGS[@]}"
	run_test "nvmf_rpc" $rootdir/test/nvmf/target/rpc.sh "${TEST_ARGS[@]}"
	run_test "nvmf_invalid" $rootdir/test/nvmf/target/invalid.sh "${TEST_ARGS[@]}"
	run_test "nvmf_connect_stress" $rootdir/test/nvmf/target/connect_stress.sh "${TEST_ARGS[@]}"
	run_test "nvmf_fused_ordering" $rootdir/test/nvmf/target/fused_ordering.sh "${TEST_ARGS[@]}"
	run_test "nvmf_ns_masking" test/nvmf/target/ns_masking.sh "${TEST_ARGS[@]}"
	if [[ $SPDK_TEST_NVME_CLI -eq 1 ]]; then
		run_test "nvmf_nvme_cli" $rootdir/test/nvmf/target/nvme_cli.sh "${TEST_ARGS[@]}"
	fi
	if [[ $SPDK_TEST_VFIOUSER -eq 1 ]]; then
		run_test "nvmf_vfio_user" $rootdir/test/nvmf/target/nvmf_vfio_user.sh "${TEST_ARGS[@]}"
		run_test "nvmf_vfio_user_nvme_compliance" $rootdir/test/nvme/compliance/compliance.sh "${TEST_ARGS[@]}"
		run_test "nvmf_vfio_user_fuzz" $rootdir/test/nvmf/target/vfio_user_fuzz.sh "${TEST_ARGS[@]}"
	fi
fi

run_test "nvmf_auth_target" "$rootdir/test/nvmf/target/auth.sh" "${TEST_ARGS[@]}"

if [ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" ]; then
	run_test "nvmf_bdevio_no_huge" $rootdir/test/nvmf/target/bdevio.sh "${TEST_ARGS[@]}" --no-hugepages
	run_test "nvmf_tls" $rootdir/test/nvmf/target/tls.sh "${TEST_ARGS[@]}"
	run_test "nvmf_fips" $rootdir/test/nvmf/fips/fips.sh "${TEST_ARGS[@]}"
	run_test "nvmf_control_msg_list" $rootdir/test/nvmf/target/control_msg_list.sh "${TEST_ARGS[@]}"
	run_test "nvmf_wait_for_buf_clean_flow" $rootdir/test/nvmf/target/wait_for_buf.sh clean_flow "${TEST_ARGS[@]}"
	run_test "nvmf_wait_for_buf_dirty_flow" $rootdir/test/nvmf/target/wait_for_buf.sh dirty_flow "${TEST_ARGS[@]}"
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "nvmf_fuzz" $rootdir/test/nvmf/target/fabrics_fuzz.sh "${TEST_ARGS[@]}"
	run_test "nvmf_multiconnection" $rootdir/test/nvmf/target/multiconnection.sh "${TEST_ARGS[@]}"
	run_test "nvmf_initiator_timeout" $rootdir/test/nvmf/target/initiator_timeout.sh "${TEST_ARGS[@]}"
fi

if [[ $NET_TYPE == phy ]]; then
	if [ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" ]; then
		gather_supported_nvmf_pci_devs
		TCP_INTERFACE_LIST=("${net_devs[@]}")
		if ((${#TCP_INTERFACE_LIST[@]} > 0)); then
			run_test "nvmf_perf_adq" $rootdir/test/nvmf/target/perf_adq.sh "${TEST_ARGS[@]}"
		fi
	elif [[ $SPDK_TEST_NVMF_TRANSPORT == "rdma" ]]; then
		# Disabled due to https://github.com/spdk/spdk/issues/3345
		# run_test "nvmf_device_removal" test/nvmf/target/device_removal.sh "${TEST_ARGS[@]}"
		run_test "nvmf_srq_overwhelm" "$rootdir/test/nvmf/target/srq_overwhelm.sh" "${TEST_ARGS[@]}"
	fi
	run_test "nvmf_shutdown" $rootdir/test/nvmf/target/shutdown.sh "${TEST_ARGS[@]}"
fi
run_test "nvmf_nsid" "$rootdir/test/nvmf/target/nsid.sh" "${TEST_ARGS[@]}"

trap - SIGINT SIGTERM EXIT
