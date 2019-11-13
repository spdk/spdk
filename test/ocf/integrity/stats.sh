#! /usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/test/bdev/bdevperf/bdevperf
rpc_py="$rootdir/scripts/rpc.py"

$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -w write -t 120 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
sleep 1
$rpc_py bdev_ocf_get_stats MalCache1
killprocess $bdev_perf_pid

