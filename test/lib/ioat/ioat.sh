#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

timing_enter ioat

timing_enter perf
$rootdir/examples/ioat/perf/perf -t 1
timing_exit perf

timing_enter verify
$rootdir/examples/ioat/verify/verify -t 1
timing_exit verify

if [ `uname` = Linux ]; then
	timing_enter kperf
	$rootdir/scripts/setup.sh reset
	insmod $rootdir/examples/ioat/kperf/kmod/dmaperf.ko
	$rootdir/examples/ioat/kperf/ioat_kperf -n 4 -q 4 -s 12 -t 32
	rmmod dmaperf.ko
	$rootdir/scripts/setup.sh
	report_test_completion "ioat_kperf"
	timing_exit kperf
fi

report_test_completion "ioat"
timing_exit ioat
