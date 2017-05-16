#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

if [ $(uname -s) = Linux ]; then
        timing_enter nvme_mp

	timing_enter mp_func_test
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 5 -c 0xf &
	sleep 3
	$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 1 -c 0x10
	wait $!
	timing_exit mp_func_test

	timing_enter mp_fault_test
	timing_enter mp_fault_test_1
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 5 -c 0xf &
	pid=$!
	sleep 3
	$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 5 -c 0x10 &
	sleep 1
	kill -9 $pid
	wait $!
	timing_exit mp_fault_test_1

	timing_enter mp_fault_test_2
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 7 -c 0xf &
	pid=$!
	sleep 3
	$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 3 -c 0x10 &
	sleep 2
	kill -9 $!
	wait $pid
	timing_exit mp_fault_test_2
	timing_exit mp_fault_test

	timing_enter mp_stress_test
	timing_enter mp_stress_test_1
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 10 -c 0xf &
	sleep 3
	count=0
	while [ $count -le 4 ]; do
		$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 1 -c 0x10
		count=$(( $count + 1 ))
	done
	wait $!
	timing_exit mp_stress_test_1

	timing_enter mp_stress_test_2
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 15 -c 0xf &
	pid=$!
	sleep 3
	count=0
	while [ $count -le 4 ]; do
		$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 3 -c 0x10 &
		sleep 2
		kill -9 $!
		count=$(( $count + 1 ))
	done
	wait $pid
	timing_exit mp_stress_test_2

	timing_enter mp_stress_test_3
	$rootdir/examples/nvme/arbitration/arbitration -i 0 -s 4096 -t 10 &
	pid=$!
	sleep 3
	count=0
	while [ $count -le 4 ]; do
		core=$((1 << (($count + 4))))
		printf -v hexcore "0x%x" "$core"
		$rootdir/examples/nvme/perf/perf -i 0 -q 128 -w read -s 4096 -t 1 -c $hexcore &
		count=$(( $count + 1 ))
	done
	wait $pid
	timing_exit mp_stress_test_3
	timing_exit mp_stress_test

	timing_enter mp_perf_test
	$rootdir/examples/nvme/perf/perf -i 0 -q 1 -w randread -s 4096 -t 5 -c 0x3
	sleep 3

	$rootdir/examples/nvme/perf/perf -i 0 -q 1 -w randread -s 4096 -t 8 -c 0x1 &
	sleep 3
	$rootdir/examples/nvme/perf/perf -i 0 -q 1 -w randread -s 4096 -t 3 -c 0x2
	wait $!
	timing_exit mp_perf_test

        timing_exit nvme_mp
fi
