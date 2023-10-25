#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

accel_perf() {
	"$SPDK_EXAMPLE_DIR/accel_perf" -c <(build_accel_config) "$@"
}

build_accel_config() {
	accel_json_cfg=()
	[[ $SPDK_TEST_ACCEL_DSA -gt 0 ]] && accel_json_cfg+=('{"method": "dsa_scan_accel_module"}')
	[[ $SPDK_TEST_ACCEL_IAA -gt 0 ]] && accel_json_cfg+=('{"method": "iaa_scan_accel_module"}')
	[[ $SPDK_TEST_ACCEL_IOAT -gt 0 ]] && accel_json_cfg+=('{"method": "ioat_scan_accel_module"}')

	if [[ $COMPRESSDEV ]]; then
		accel_json_cfg+=('{"method": "compressdev_scan_accel_module", "params":{"pmd": 0}}')
	fi

	local IFS=","
	jq -r '.' <<- JSON
		{
		 "subsystems": [
		  {
		   "subsystem": "accel",
		   "config": [
		   ${accel_json_cfg[*]}
		   ]
		  }
		 ]
		}
	JSON
}

# Run some additional simple tests; mostly focused
# around accel_perf example app itself, not necessarily
# testing actual accel modules.
# Cover the help message. Not really a test, but
# it doesn't fit anywhere else.
run_test "accel_help" accel_perf -h &> /dev/null
# Filename required for compress operations
run_test "accel_missing_filename" NOT accel_perf -t 1 -w compress
# Compression does not support verify option
run_test "accel_compress_verify" NOT accel_perf -t 1 -w compress -l $testdir/bib -y
# Trigger error by specifying wrong workload type
run_test "accel_wrong_workload" NOT accel_perf -t 1 -w foobar
# Use negative number for source buffers parameters
run_test "accel_negative_buffers" NOT accel_perf -t 1 -w xor -y -x -1

#Run through all SW ops with defaults for a quick sanity check
#To save time, only use verification case
run_test "accel_crc32c" accel_perf -t 1 -w crc32c -y
run_test "accel_crc32c_C2" accel_perf -t 1 -w crc32c -y -C 2
run_test "accel_copy" accel_perf -t 1 -w copy -y
run_test "accel_fill" accel_perf -t 1 -w fill -y
run_test "accel_copy_crc32c" accel_perf -t 1 -w copy_crc32c -y
run_test "accel_copy_crc32c_C2" accel_perf -t 1 -w copy_crc32c -y -C 2
run_test "accel_dualcast" accel_perf -t 1 -w dualcast -y
run_test "accel_compare" accel_perf -t 1 -w compare -y
run_test "accel_xor" accel_perf -t 1 -w xor -y
run_test "accel_xor" accel_perf -t 1 -w xor -y -x 3
# do not run compress/decompress unless ISAL is installed
if [[ $CONFIG_ISAL == y ]]; then
	run_test "accel_comp" accel_perf -t 1 -w compress -l $testdir/bib
	run_test "accel_decomp" accel_perf -t 1 -w decompress -l $testdir/bib -y
	run_test "accel_decmop_full" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0
	run_test "accel_decomp_mcore" accel_perf -t 1 -w decompress -l $testdir/bib -y -m 0xf
	run_test "accel_decomp_full_mcore" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -m 0xf
	run_test "accel_decomp_mthread" accel_perf -t 1 -w decompress -l $testdir/bib -y -T 2
	run_test "accel_deomp_full_mthread" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -T 2
fi
if [[ $CONFIG_DPDK_COMPRESSDEV == y ]]; then
	COMPRESSDEV=1
	run_test "accel_cdev_comp" accel_perf -t 1 -w compress -l $testdir/bib
	run_test "accel_cdev_decomp" accel_perf -t 1 -w decompress -l $testdir/bib -y
	run_test "accel_cdev_decmop_full" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0
	run_test "accel_cdev_decomp_mcore" accel_perf -t 1 -w decompress -l $testdir/bib -y -m 0xf
	run_test "accel_cdev_decomp_full_mcore" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -m 0xf
	run_test "accel_cdev_decomp_mthread" accel_perf -t 1 -w decompress -l $testdir/bib -y -T 2
	run_test "accel_cdev_deomp_full_mthread" accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -T 2
	unset COMPRESSDEV
fi

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

$SPDK_BIN_DIR/spdk_tgt --wait-for-rpc &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

$rpc_py accel_assign_opc -o copy -m software
$rpc_py framework_start_init
$rpc_py accel_get_opc_assignments | jq -r '.copy' | grep software

killprocess $spdk_tgt_pid
