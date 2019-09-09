#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

tests=('-q 1 -w randwrite -t 4 -o 69632' '-q 128 -w randwrite -t 4 -o 4096' '-q 128 -w verify -t 4 -o 4096')
device=$1
rpc_py=$rootdir/scripts/rpc.py

for (( i=0; i<${#tests[@]}; i++ )) do
	timing_enter "${tests[$i]}"
	$rootdir/test/bdev/bdevperf/bdevperf -z -T ftl0 ${tests[$i]} & bdevperf_pid=$!
	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	$rpc_py construct_nvme_bdev -b nvme0 -a $device -m ocssd -t pcie
	$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1
	$rpc_py construct_ftl_bdev -b ftl0 -d nvme0n1
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	killprocess $bdevperf_pid
	timing_exit "${tests[$i]}"
done

report_test_completion ftl_bdevperf
trap - SIGINT SIGTERM EXIT
