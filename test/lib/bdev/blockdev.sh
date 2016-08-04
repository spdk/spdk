#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

testdir=$(readlink -f $(dirname $0))

timing_enter blockdev

timing_enter bounds
$testdir/bdevio/bdevio $testdir/bdev.conf
process_core
timing_exit bounds

timing_enter verify
$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 32 -s 4096 -w verify -t 5
process_core
timing_exit verify

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Use size 192KB which both exceeds typical 128KB max NVMe I/O
	#  size and will cross 128KB Intel DC P3700 stripe boundaries.
	timing_enter perf
	$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -w read -s 196608 -t 5
	process_core
	timing_exit perf

	timing_enter reset
	$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	process_core
	timing_exit reset

	timing_enter unmap
	$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 1 -w unmap -s 4096 -t 60
	process_core
	timing_exit unmap
fi

timing_exit blockdev
