#!/usr/bin/env bash

SYSTEM=$(uname -s)
if [ $SYSTEM = "FreeBSD" ]; then
	echo "blob_io_wait.sh cannot run on FreeBSD currently."
	exit 0
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
rpc_py="$rootdir/scripts/rpc.py"

truncate -s 64M $testdir/aio.bdev

$rootdir/test/app/bdev_svc/bdev_svc --wait-for-rpc &
bdev_svc_pid=$!

trap 'rm -f $testdir/bdevperf.json; rm -f $testdir/aio.bdev; killprocess $bdev_svc_pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $bdev_svc_pid
# Minimal number of bdev io pool (128) and cache (1)
$rpc_py bdev_set_options --bdev-io-pool-size 128 --bdev-io-cache-size 1 --small-buf-pool-size 8192 --large-buf-pool-size 1024
$rpc_py framework_start_init
$rpc_py bdev_aio_create $testdir/aio.bdev aio0 4096
$rpc_py bdev_lvol_create_lvstore aio0 lvs0
$rpc_py bdev_lvol_create -l lvs0 lvol0 32
$rpc_py save_config > $testdir/bdevperf.json

killprocess $bdev_svc_pid

$rootdir/test/bdev/bdevperf/bdevperf --json $testdir/bdevperf.json -q 128 -o 4096 -w write -t 5 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
$rpc_py bdev_enable_histogram aio0 -e
sleep 2
$rpc_py bdev_get_histogram aio0 | $rootdir/scripts/histogram.py
$rpc_py bdev_enable_histogram aio0 -d
wait $bdev_perf_pid

$rootdir/test/bdev/bdevperf/bdevperf --json $testdir/bdevperf.json -q 128 -o 4096 -w read -t 5 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
$rpc_py bdev_enable_histogram aio0 -e
sleep 2
$rpc_py bdev_get_histogram aio0 | $rootdir/scripts/histogram.py
$rpc_py bdev_enable_histogram aio0 -d
wait $bdev_perf_pid

$rootdir/test/bdev/bdevperf/bdevperf --json $testdir/bdevperf.json -q 128 -o 4096 -w unmap -t 1

sync
rm -f $testdir/bdevperf.json
rm -f $testdir/aio.bdev
trap - SIGINT SIGTERM EXIT
