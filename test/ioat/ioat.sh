#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter ioat

timing_enter perf
$rootdir/examples/ioat/perf/ioat_perf -t 1
timing_exit perf

timing_enter verify
$rootdir/examples/ioat/verify/verify -t 1
timing_exit verify

report_test_completion "ioat"
timing_exit ioat
