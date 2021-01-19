#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

#Test case 1: crc32c test
#To save time, only use verification case
run_test "accel_engine" $SPDK_EXAMPLE_DIR/accel_perf -q 128 -o 4096 -t 2 -w crc32c -y
