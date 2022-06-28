#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

tests=('-q 1 -w randwrite -t 4 -o 69632' '-q 128 -w randwrite -t 4 -o 4096' '-q 128 -w verify -t 4 -o 4096')
device=$1
cache_device=$2
use_append=$3
rpc_py=$rootdir/scripts/rpc.py
timeout=240

for ((i = 0; i < ${#tests[@]}; i++)); do
	timing_enter "${tests[$i]}"
	"$rootdir/test/bdev/bdevperf/bdevperf" -z -T ftl0 ${tests[$i]} &
	bdevperf_pid=$!

	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	split_bdev=$(create_base_bdev nvme0 $device $((1024 * 101)))
	nv_cache=$(create_nv_cache_bdev nvc0 $cache_device $split_bdev)

	$rpc_py -t $timeout bdev_ftl_create -b ftl0 -d $split_bdev $use_append -c $nv_cache

	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	$rpc_py bdev_ftl_delete -b ftl0

	$rpc_py bdev_nvme_detach_controller nvme0
	killprocess $bdevperf_pid
	trap - SIGINT SIGTERM EXIT
	timing_exit "${tests[$i]}"
done

remove_shm
