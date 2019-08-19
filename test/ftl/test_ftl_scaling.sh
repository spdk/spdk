#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

#tests=('-q 1 -w randwrite -t 20 -q 128 -o 131072' '-q 128 -w randwrite -t 4 -o 4096' '-q 128 -w verify -t 4 -o 4096')
#tests=('-w write -t 10 -q 128 -o 131072')
#tests=('-w write -t 10 -q 128 -o 65536')
tests=('-w randwrite -t 5 -q 128 -o 4096')
devices=(\
'0000:5e:00.0' \
'0000:d9:00.0' \
'0000:d8:00.0' \
'0000:5f:00.0' \
#'0000:1d:00.0' \
#'0000:1e:00.0' \
#'0000:1a:00.0' \
#'0000:21:00.0' \
#'0000:24:00.0' \
#'0000:26:00.0' \
#'0000:29:00.0' \
#'0000:2b:00.0' \
#'0000:b1:00.0' \
#'0000:b2:00.0' \
#'0000:b3:00.0' \
#'0000:b4:00.0' \
)
rpc_py=$rootdir/scripts/rpc.py

ftl_bdev_conf=$testdir/config/ftl.conf
gen_ftl_nvme_conf > $ftl_bdev_conf

for (( i=0; i<${#tests[@]}; i++ )) do
	timing_enter "${tests[$i]}"
	$rootdir/test/bdev/bdevperf/bdevperf -m 3 -C -z -T ftl0 ${tests[$i]} -c $ftl_bdev_conf &
	bdevperf_pid=$!

	trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid

	nvme_ctrls=""
	for (( j=0; j<${#devices[@]}; j++ )) do
		nvme_ctrls+="nvme${j}n1 "
		$rpc_py bdev_nvme_attach_controller -b nvme${j} -a ${devices[$j]} -t pcie
	done

	$rpc_py bdev_raid_create -z 64 -r 0 -b "$nvme_ctrls" -n raid0
	$rpc_py bdev_zone_block_create -z 262144 -o 1 -b zone0 -n raid0
	$rpc_py bdev_ftl_create -b ftl0 -d zone0
	#$rpc_py save_config > config.json
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	$rpc_py delete_ftl_bdev -b ftl0
	$rpc_py bdev_zone_block_delete zone0
	$rpc_py delete_nvme_controller nvme0
	killprocess $bdevperf_pid
	trap - SIGINT SIGTERM EXIT
	timing_exit "${tests[$i]}"
done

rm -f $ftl_bdev_conf
report_test_completion ftl_bdevperf
