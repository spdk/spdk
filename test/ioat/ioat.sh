#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter ioat

timing_enter perf
$rootdir/examples/ioat/perf/perf -t 1
timing_exit perf

timing_enter verify
$rootdir/examples/ioat/verify/verify -t 1
timing_exit verify

report_test_completion "ioat"
timing_exit ioat
