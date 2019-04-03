#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

spdk_pid='?'
function start_spdk()
{
	$rootdir/app/iscsi_tgt/iscsi_tgt &
	spdk_pid=$!
	trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid
}
function stop_spdk()
{
	killprocess $spdk_pid
	trap - SIGINT SIGTERM EXIT
}

start_spdk

# Hotplug case

$rpc_py construct_malloc_bdev   1 512 -b Core0
$rpc_py construct_malloc_bdev   1 512 -b Core1

$rpc_py construct_ocf_bdev C1 wt Cache Core0
$rpc_py construct_ocf_bdev C2 wt Cache Core1

$rpc_py get_ocf_bdevs | jq -e \
	'any(select(.started)) == false'

$rpc_py construct_malloc_bdev 101 512 -b Cache

$rpc_py get_ocf_bdevs | jq -e \
	'all(select(.started)) == true'

# Detaching cores

$rpc_py  delete_ocf_bdev C2

$rpc_py get_ocf_bdevs C1 | jq -e \
	'.[0] | .started'

$rpc_py construct_ocf_bdev C2 wt Cache Core1

$rpc_py get_ocf_bdevs C2 | jq -e \
	'.[0] | .started'

# Normal shutdown

stop_spdk

# Hotremove case
start_spdk

$rpc_py construct_malloc_bdev 101 512 -b Cache
$rpc_py construct_malloc_bdev 101 512 -b Malloc
$rpc_py construct_malloc_bdev   1 512 -b Core

$rpc_py construct_ocf_bdev C1 wt Cache Malloc
$rpc_py construct_ocf_bdev C2 wt Cache Core

$rpc_py get_ocf_bdevs Cache | jq \
	'length == 2'

$rpc_py delete_malloc_bdev Cache

$rpc_py get_ocf_bdevs | jq -e \
	'. == []'

# Not fully initialized shutdown

$rpc_py construct_ocf_bdev C1 wt Malloc NonExisting
$rpc_py construct_ocf_bdev C2 wt Malloc NonExisting
$rpc_py construct_ocf_bdev C3 wt Malloc Core

stop_spdk
