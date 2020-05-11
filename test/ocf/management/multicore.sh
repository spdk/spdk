#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

spdk_pid='?'
function start_spdk() {
	$SPDK_BIN_DIR/iscsi_tgt &
	spdk_pid=$!
	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid
}
function stop_spdk() {
	killprocess $spdk_pid
	trap - SIGINT SIGTERM EXIT
}

start_spdk

# Hotplug case

$rpc_py bdev_malloc_create 1 512 -b Core0
$rpc_py bdev_malloc_create 1 512 -b Core1

$rpc_py bdev_ocf_create C1 wt Cache Core0
$rpc_py bdev_ocf_create C2 wt Cache Core1

$rpc_py bdev_ocf_get_bdevs | jq -e \
	'any(select(.started)) == false'

$rpc_py bdev_malloc_create 101 512 -b Cache

$rpc_py bdev_ocf_get_bdevs | jq -e \
	'all(select(.started)) == true'

#Be sure that we will not fail delete because examine is still in progress
waitforbdev C2

# Detaching cores

$rpc_py bdev_ocf_delete C2

$rpc_py bdev_ocf_get_bdevs C1 | jq -e \
	'.[0] | .started'

$rpc_py bdev_ocf_create C2 wt Cache Core1

$rpc_py bdev_ocf_get_bdevs C2 | jq -e \
	'.[0] | .started'

# Normal shutdown

stop_spdk

# Hotremove case
start_spdk

$rpc_py bdev_malloc_create 101 512 -b Cache
$rpc_py bdev_malloc_create 101 512 -b Malloc
$rpc_py bdev_malloc_create 1 512 -b Core

$rpc_py bdev_ocf_create C1 wt Cache Malloc
$rpc_py bdev_ocf_create C2 wt Cache Core

$rpc_py bdev_ocf_get_bdevs Cache | jq \
	'length == 2'

$rpc_py bdev_malloc_delete Cache

$rpc_py bdev_ocf_get_bdevs | jq -e \
	'. == []'

# Not fully initialized shutdown

$rpc_py bdev_ocf_create C1 wt Malloc NonExisting
$rpc_py bdev_ocf_create C2 wt Malloc NonExisting
$rpc_py bdev_ocf_create C3 wt Malloc Core

stop_spdk
