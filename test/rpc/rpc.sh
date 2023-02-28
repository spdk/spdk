#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
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

function rpc_plugins() {
	time {
		malloc=$($rpc --plugin rpc_plugin create_malloc)
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "1" ]

		$rpc --plugin rpc_plugin delete_malloc $malloc
		bdevs=$($rpc bdev_get_bdevs)
		[ "$(jq length <<< "$bdevs")" == "0" ]
	}
}

function rpc_trace_cmd_test() {
	local info

	info=$($rpc trace_get_info)
	[ "$(jq length <<< "$info")" -gt 2 ]
	[ "$(jq 'has("tpoint_group_mask")' <<< "$info")" = "true" ]
	[ "$(jq 'has("tpoint_shm_path")' <<< "$info")" = "true" ]
	[ "$(jq 'has("bdev")' <<< "$info")" = "true" ]
	[ "$(jq -r .bdev.tpoint_mask <<< "$info")" != "0x0" ]
}

$SPDK_BIN_DIR/spdk_tgt -e bdev &
spdk_pid=$!
trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

export PYTHONPATH=$PYTHONPATH:$testdir

# basic integrity test
rpc=rpc_cmd
run_test "rpc_integrity" rpc_integrity
run_test "rpc_plugins" rpc_plugins
run_test "rpc_trace_cmd_test" rpc_trace_cmd_test
# same integrity test, but with rpc_cmd() instead
rpc="rpc_cmd"
run_test "rpc_daemon_integrity" rpc_integrity

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
