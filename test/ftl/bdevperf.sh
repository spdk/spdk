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
zone_size=$[262144]
write_unit_size=16
timeout=240

for ((i = 0; i < ${#tests[@]}; i++)); do
	timing_enter "${tests[$i]}"
	"$rootdir/test/bdev/bdevperf/bdevperf" -z -T ftl0 ${tests[$i]} --json <(gen_ftl_nvme_conf) &
	bdevperf_pid=$!

	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
	split_bdev=$($rootdir/scripts/rpc.py bdev_split_create nvme0n1 -s $((1024*101))  1)
	nv_cache=$(create_nv_cache_bdev nvc0 $cache_device $split_bdev)

	$rpc_py -t $timeout bdev_ftl_create -b ftl0 -d $split_bdev $use_append -c $nv_cache

	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	$rpc_py delete_ftl_bdev -b ftl0

	$rpc_py bdev_nvme_detach_controller nvme0
	killprocess $bdevperf_pid
	trap - SIGINT SIGTERM EXIT
	timing_exit "${tests[$i]}"
done

remove_shm
