#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Dell Inc, or its subsidiaries.
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"
source "$rootdir/test/scheduler/common.sh"
source "$rootdir/test/interrupt/common.sh"

CPU_UTIL_INTR_THRESHOLD=10
NQN=nqn.2016-06.io.spdk:cnode$$
BDEVPERF_RPC_SOCK=/var/tmp/bdevperf.sock

bdev_nvme_attach_ctrlr() {
	"$rpc_py" -s "$BDEVPERF_RPC_SOCK" bdev_nvme_attach_controller \
		-b Nvme0 \
		-t "$TEST_TRANSPORT" \
		-a "$NVMF_FIRST_TARGET_IP" \
		-f ipv4 \
		-s "$NVMF_PORT" \
		-n "$NQN"
}

nvme_rdma_intr_mode() {
	local cpu_util_pre cpu_util_post cpu_util_io
	local bdevperf_pid bdevperfpy_pid

	nvmftestinit
	nvmfappstart -m 0x2
	setup_bdev_aio

	"$rpc_py" nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -q 256
	"$rpc_py" nvmf_create_subsystem "$NQN" -a -s "$NVMF_SERIAL"
	"$rpc_py" nvmf_subsystem_add_ns "$NQN" AIO0
	"$rpc_py" nvmf_subsystem_add_listener "$NQN" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	"$SPDK_EXAMPLE_DIR/bdevperf" -z -q 1 -o 262144 -t 10 -w read -m 0x1 \
		-r "$BDEVPERF_RPC_SOCK" --interrupt-mode &
	bdevperf_pid=$!

	waitforlisten "$bdevperf_pid" "$BDEVPERF_RPC_SOCK"
	trap 'killprocess "$bdevperf_pid"; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

	bdev_nvme_attach_ctrlr
	cpu_util_pre=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 5 0)

	"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$BDEVPERF_RPC_SOCK" perform_tests &
	bdevperfpy_pid=$!
	sleep 1

	cpu_util_io=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 8 0)
	wait "$bdevperfpy_pid"
	cpu_util_post=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 5 0)

	trap - SIGINT SIGTERM EXIT
	killprocess "$bdevperf_pid"
	"$rpc_py" nvmf_delete_subsystem "$NQN"
	nvmftestfini

	cat <<- SUMMARY
		pre CPU util: $cpu_util_pre
		IO CPU util: $cpu_util_io
		post CPU util: $cpu_util_post
	SUMMARY

	((cpu_util_pre < CPU_UTIL_INTR_THRESHOLD && cpu_util_post < CPU_UTIL_INTR_THRESHOLD))
}

run_test "nvme_rdma_intr_mode" nvme_rdma_intr_mode
