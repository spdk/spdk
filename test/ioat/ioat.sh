#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "ioat_perf" $rootdir/examples/ioat/perf/ioat_perf -t 1

run_test "ioat_verify" $rootdir/examples/ioat/verify/verify -t 1
