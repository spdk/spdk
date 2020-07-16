#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

function bdev_check_claimed() {
	if [ "$($rpc_py get_bdevs -b "$@" | jq '.[0].claimed')" = "true" ]; then
		return 0
	else
		return 1
	fi
}

$SPDK_BIN_DIR/iscsi_tgt &
spdk_pid=$!

trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

$rpc_py bdev_malloc_create 101 512 -b Malloc0
$rpc_py bdev_malloc_create 101 512 -b Malloc1

$rpc_py bdev_ocf_create PartCache wt Malloc0 NonExisting

$rpc_py bdev_ocf_get_bdevs PartCache | jq -e \
	'.[0] | .started == false and .cache.attached and .core.attached == false'

$rpc_py bdev_ocf_get_bdevs NonExisting | jq -e \
	'.[0] | .name == "PartCache"'

if ! bdev_check_claimed Malloc0; then
	echo >&2 "Base device expected to be claimed now"
	exit 1
fi

$rpc_py bdev_ocf_delete PartCache
if bdev_check_claimed Malloc0; then
	echo >&2 "Base device is not expected to be claimed now"
	exit 1
fi

$rpc_py bdev_ocf_create FullCache wt Malloc0 Malloc1 --cache-line-size 8

$rpc_py bdev_ocf_get_bdevs FullCache | jq -e \
	'.[0] | .started and .cache.attached and .core.attached'

if ! (bdev_check_claimed Malloc0 && bdev_check_claimed Malloc1); then
	echo >&2 "Base devices expected to be claimed now"
	exit 1
fi

$rpc_py bdev_ocf_delete FullCache
if bdev_check_claimed Malloc0 && bdev_check_claimed Malloc1; then
	echo >&2 "Base devices are not expected to be claimed now"
	exit 1
fi

$rpc_py bdev_ocf_create HotCache wt Malloc0 Malloc1 --cache-line-size 16

if ! (bdev_check_claimed Malloc0 && bdev_check_claimed Malloc1); then
	echo >&2 "Base devices expected to be claimed now"
	exit 1
fi

$rpc_py bdev_malloc_delete Malloc0

if bdev_check_claimed Malloc1; then
	echo >&2 "Base device is not expected to be claimed now"
	exit 1
fi

status=$($rpc_py get_bdevs)
gone=$(echo $status | jq 'map(select(.name == "HotCache")) == []')
if [[ $gone == false ]]; then
	echo >&2 "OCF bdev is expected to unregister"
	exit 1
fi

# check if shutdown of running CAS bdev is ok
$rpc_py bdev_ocf_create PartCache wt NonExisting Malloc1

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
