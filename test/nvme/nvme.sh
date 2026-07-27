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

function check_mixed_output() {
	local out=$1

	grep -q "Per-workload breakdown:" <<< "$out"
	awk '
		$1 == "4096B" && $2 == "randread" { small = $6 }
		$1 == "12288B" && $2 == "randwrite" { large = $6 }
		END { exit !(small > 0 && large > 0 && small < large && small / large > 0.5) }
	' <<< "$out"
}

function nvme_perf_mixed_workload_invalid_args() {
	local perf=$SPDK_BIN_DIR/spdk_nvme_perf

	# Each case must fail on argument validation alone. Run this only when no
	# stub holds the controllers, otherwise NOT would also pass on a probe failure.
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:30,131072:randwrite:30'
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:60,131072:randwrite:60'
	NOT $perf -q 1 -t 1 --mixed-workload '4096:bogus:100'
	NOT $perf -q 1 -t 1 --mixed-workload '4kzz:randread:100'
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:0'
	NOT $perf -q 1 -t 1 --mixed-workload '4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:6,4096:read:4'
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:100' -o 4096
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:100' -w randread
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:100' -M 50
	# -F/--zipf requires every random workload to share one IO size.
	NOT $perf -q 1 -t 1 --mixed-workload '4096:randread:50,131072:randwrite:50' -F 1.2
}

function nvme_perf_mixed_workload_nvme() {
	local perf=$SPDK_BIN_DIR/spdk_nvme_perf out

	# Fail explicitly rather than silently skip if there is no local NVMe.
	get_nvme_bdfs > /dev/null

	# Blended sizes and workloads, exercising suffix parsing, multi-iov
	# submission, and explicit-versus-legacy breakdown output.
	$perf -q 32 -t 1 -i 0 --mixed-workload '4K:randread:70,128k:randwrite:30'
	$perf -q 32 -t 1 -i 0 -O 16384 --mixed-workload '4096:randread:50,256K:write:50'
	$perf -q 32 -t 1 -i 0 -F 1.2 --mixed-workload '4096:randread:50,4096:randwrite:50'
	out=$($perf -q 32 -t 1 -i 0 --mixed-workload '4096:read:60,8192:write:40')
	grep -q "Per-workload breakdown:" <<< "$out"
	out=$($perf -q 32 -t 1 -i 0 -o 4096 -w randrw -M 50)
	NOT grep -q "Per-workload breakdown:" <<< "$out"
}

# Exercise the AIO (or, with -R, io_uring) backend on a zram device.
function nvme_perf_mixed_workload_zram() {
	local perf=$SPDK_BIN_DIR/spdk_nvme_perf zram_dev_id dev out block_size dev_size io_size

	init_zram
	zram_dev_id=$(create_zram_dev)
	set_zram_dev "$zram_dev_id" 64M
	dev=/dev/zram$zram_dev_id
	block_size=$(blockdev --getss "$dev")
	dev_size=$(blockdev --getsize64 "$dev")

	# These only warn and drop the device, so perf may exit non-zero if no
	# other device is left; check the warning, not the exit status.
	io_size=$((block_size + 1))
	out=$($perf "$dev" "$@" -q 1 -t 1 -i 0 --mixed-workload "$io_size:read:100" 2>&1 || true)
	grep -Fq "IO size $io_size (workload 0) is not a multiple of device $dev block size $block_size" <<< "$out"

	io_size=$((dev_size + block_size))
	out=$($perf "$dev" "$@" -q 1 -t 1 -i 0 --mixed-workload "$io_size:read:100" 2>&1 || true)
	grep -Fq "IO size $io_size (workload 0) is larger than device $dev size $dev_size" <<< "$out"

	out=$($perf "$dev" "$@" -q 32 -t 1 -i 0 --mixed-workload '4096:randread:70,12288:randwrite:30')
	check_mixed_output "$out"

	remove_zram_dev "$zram_dev_id"
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
run_test "nvme_perf_mixed_workload_nvme" nvme_perf_mixed_workload_nvme
if [[ $(uname -s) == Linux ]]; then
	run_test "nvme_perf_mixed_workload_aio" nvme_perf_mixed_workload_zram
	if [[ $SPDK_TEST_URING -eq 1 ]]; then
		run_test "nvme_perf_mixed_workload_uring" nvme_perf_mixed_workload_zram -R
	fi
fi
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

run_test "nvme_perf_mixed_workload_invalid_args" nvme_perf_mixed_workload_invalid_args

run_test "bdev_nvme_reset_stuck_adm_cmd" $rootdir/test/nvme/nvme_reset_stuck_adm_cmd.sh

if [[ $CONFIG_FIO_PLUGIN == y ]]; then
	run_test "nvme_fio" nvme_fio_test
fi
