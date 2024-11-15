#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Samsung Electronics Co., Ltd.
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/scheduler/common.sh"

CPU_UTIL_INTR_THRESHOLD=10
CPU_UTIL_POLL_THRESHOLD=95

bdev_nvme_attach_ctrlr() {
	rpc_cmd bdev_nvme_attach_controller --name Nvme0 --trtype PCIe --traddr "$1"
}

# Abort the test in case no nvmes are found
nvmes=($(nvme_in_userspace))
nvme=${nvmes[0]}

# Abort here as vfio-pci is hard dependency for interrupts
[[ -e /sys/bus/pci/drivers/vfio-pci/$nvme/vfio-dev ]]

nvme_pcie_intr_mode() {
	local cpu_util_pre cpu_util_post cpu_util_io
	local bdevperfpy_pid
	$rootdir/build/examples/bdevperf -z -q 1 -o 262144 -t 10 -w read -m 0x1 --interrupt-mode &
	bdevperf_pid=$!

	waitforlisten $bdevperf_pid
	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT

	bdev_nvme_attach_ctrlr "$nvme"
	cpu_util_pre=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 5 0)

	$rootdir/examples/bdev/bdevperf/bdevperf.py perform_tests &
	bdevperfpy_pid=$!
	sleep 1

	cpu_util_io=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 8 0)
	wait "$bdevperfpy_pid"
	cpu_util_post=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 5 0)

	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid

	cat <<- SUMMARY
		pre CPU util: $cpu_util_pre
		IO CPU util: $cpu_util_io
		post CPU util: $cpu_util_post
	SUMMARY

	# Make sure that the main expectation of having low cpu load before and after tests is met.
	# FIXME: Measuring cpu load during IO seems to be flaky and dependent on multiple factors.
	# It's been noticed that even change in SPDK's cpu mask, io size, etc. may significantly
	# impact the result. With that in mind, we verify only the base and for perform_tests we
	# simply report the overall CPU usage for debugging purposes.
	((cpu_util_pre < CPU_UTIL_INTR_THRESHOLD && cpu_util_post < CPU_UTIL_INTR_THRESHOLD))
}

nvme_pcie_poll_mode() {
	local cpu_util
	$rootdir/build/examples/bdevperf -z -q 1 -o 262144 -t 10 -w read -m 0x1 &
	bdevperf_pid=$!

	waitforlisten $bdevperf_pid
	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT

	bdev_nvme_attach_ctrlr "$nvme"
	$rootdir/examples/bdev/bdevperf/bdevperf.py perform_tests &
	sleep 1

	cpu_util=$(spdk_pid=$bdevperf_pid get_spdk_proc_time 8 0)

	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid

	if ((cpu_util < CPU_UTIL_POLL_THRESHOLD)); then
		return 1
	fi
}

run_test "nvme_pcie_intr_mode" nvme_pcie_intr_mode
run_test "nvme_pcie_poll_mode" nvme_pcie_poll_mode
