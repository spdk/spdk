#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

#Run through all SW ops with defaults for a quick sanity check
#To save time, only use verification case
run_test "accel_crc32c" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w crc32c -y
run_test "accel_crc32c_C2" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w crc32c -y -C 2
run_test "accel_copy" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w copy -y
run_test "accel_fill" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w fill -y
run_test "accel_copy_crc32c" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w copy_crc32c -y
run_test "accel_copy_crc32c_C2" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w copy_crc32c -y -C 2
run_test "accel_dualcast" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w dualcast -y
run_test "accel_compare" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w compare -y
# do not run compress/decompress unless ISAL is installed
if [[ $CONFIG_ISAL == y ]]; then
	run_test "accel_comp" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w compress -l $testdir/bib
	run_test "accel_decomp" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y
	run_test "accel_decmop_full" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0
	run_test "accel_decomp_mcore" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y -m 0xf
	run_test "accel_decomp_full_mcore" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -m 0xf
	run_test "accel_decomp_mthread" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y -T 2
	run_test "accel_deomp_full_mthread" $SPDK_EXAMPLE_DIR/accel_perf -t 1 -w decompress -l $testdir/bib -y -o 0 -T 2
fi
