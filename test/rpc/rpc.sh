#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

# simply check if rpc commands have any effect on spdk
function rpc_integrity() {
	time {
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "0" ]

		malloc=$($rpc bdev_malloc_create 8 512)
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "1" ]

		$rpc bdev_passthru_create -b "$malloc" -p Passthru0
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "2" ]

		$rpc bdev_passthru_delete Passthru0
		$rpc bdev_malloc_delete $malloc
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "0" ]
	}
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

# basic integrity test
rpc="$rootdir/scripts/rpc.py"
run_test "rpc_integrity" rpc_integrity
# same test, but with rpc_cmd() instead
rpc="rpc_cmd"
run_test "rpc_daemon_integrity" rpc_integrity

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
