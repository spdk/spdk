#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "ioat_perf" $SPDK_EXAMPLE_DIR/ioat_perf -t 1

run_test "ioat_verify" $SPDK_EXAMPLE_DIR/verify -t 1
