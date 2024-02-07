#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

function nvme_identify() {
	local bdfs=() bdf
	bdfs=($(get_nvme_bdfs))
	$SPDK_BIN_DIR/spdk_nvme_identify -i 0
	for bdf in "${bdfs[@]}"; do
		$SPDK_BIN_DIR/spdk_nvme_identify -r "trtype:PCIe traddr:${bdf}" -i 0
	done
}

function nvme_perf() {
	# enable no shutdown notification option
	$SPDK_BIN_DIR/spdk_nvme_perf -q 128 -w read -o 12288 -t 1 -LL -i 0 -N
	$SPDK_BIN_DIR/spdk_nvme_perf -q 128 -w write -o 12288 -t 1 -LL -i 0
	if [ -b /dev/ram0 ]; then
		# Test perf with AIO device
		$SPDK_BIN_DIR/spdk_nvme_perf /dev/ram0 -q 128 -w read -o 12288 -t 1 -LL -i 0
	fi
}

function nvme_fio_test() {
	PLUGIN_DIR=$rootdir/app/fio/nvme
	ran_fio=false
	local bdfs=($(get_nvme_bdfs)) bdf
	for bdf in "${bdfs[@]}"; do
		if ! "$SPDK_BIN_DIR/spdk_nvme_identify" -r "trtype:PCIe traddr:${bdf}" | grep -qE "^Namespace ID:[0-9]+"; then
			continue
		fi
		if $SPDK_BIN_DIR/spdk_nvme_identify -r "trtype:PCIe traddr:${bdf}" | grep -q "Extended Data LBA"; then
			bs=4160
		else
			bs=4096
		fi
		fio_nvme $PLUGIN_DIR/example_config.fio --filename="trtype=PCIe traddr=${bdf//:/.}" --bs="$bs"
		ran_fio=true
	done
	$ran_fio || (echo "No valid NVMe drive found. Failing test." && false)
}

function nvme_multi_secondary() {
	# Primary process exits last
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 5 -c 0x1 &
	pid0=$!
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x2 &
	pid1=$!
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x4
	wait $pid0
	wait $pid1

	# Secondary process exits last
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x1 &
	pid0=$!
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x2 &
	pid1=$!
	$SPDK_BIN_DIR/spdk_nvme_perf -i 0 -q 16 -w read -o 4096 -t 5 -c 0x4
	wait $pid0
	wait $pid1
}

function nvme_doorbell_aers() {
	local bdfs=() bdf
	bdfs=($(get_nvme_bdfs))
	for bdf in "${bdfs[@]}"; do
		timeout --preserve-status 10 $testdir/doorbell_aers/doorbell_aers -r "trtype:PCIe traddr:${bdf}"
	done
}

"$rootdir/scripts/setup.sh"

if [ $(uname) = Linux ]; then
	trap "kill_stub -9; exit 1" SIGINT SIGTERM EXIT
	start_stub "-s 4096 -i 0 -m 0xE"
fi

run_test "nvme_reset" $testdir/reset/reset -q 64 -w write -o 4096 -t 5
run_test "nvme_identify" nvme_identify
run_test "nvme_perf" nvme_perf
run_test "nvme_hello_world" $SPDK_EXAMPLE_DIR/hello_world -i 0
run_test "nvme_sgl" $testdir/sgl/sgl
run_test "nvme_e2edp" $testdir/e2edp/nvme_dp
run_test "nvme_reserve" $testdir/reserve/reserve
run_test "nvme_err_injection" $testdir/err_injection/err_injection
run_test "nvme_overhead" $testdir/overhead/overhead -o 4096 -t 1 -H -i 0
run_test "nvme_arbitration" $SPDK_EXAMPLE_DIR/arbitration -t 3 -i 0
run_test "nvme_single_aen" $testdir/aer/aer -T -i 0
run_test "nvme_doorbell_aers" nvme_doorbell_aers

if [ $(uname) != "FreeBSD" ]; then
	run_test "nvme_multi_aen" $testdir/aer/aer -m -T -i 0
	run_test "nvme_startup" $testdir/startup/startup -t 1000000
	run_test "nvme_multi_secondary" nvme_multi_secondary
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi

run_test "bdev_nvme_reset_stuck_adm_cmd" $rootdir/test/nvme/nvme_reset_stuck_adm_cmd.sh

if [[ $CONFIG_FIO_PLUGIN == y ]]; then
	run_test "nvme_fio" nvme_fio_test
fi
