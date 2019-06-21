#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

spdk_tgt=$rootdir/app/spdk_tgt/spdk_tgt

$spdk_tgt &
spdk_pid=$!
trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

# check how much time it takes to execute a regular rpc.py
time $rootdir/scripts/rpc.py get_bdevs

# measure time for sending 6 RPCs with rpc.py in daemon mode
time {
	rpc_cmd get_spdk_version
	rpc_cmd get_bdevs
	malloc=$(rpc_cmd construct_malloc_bdev 8 512)
	rpc_cmd get_bdevs
	rpc_cmd construct_passthru_bdev -b "$malloc" -p Passthru0
	rpc_cmd get_bdevs
}

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
