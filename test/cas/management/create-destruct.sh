#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"

function check_claimed()
{
	status=$($rpc_py get_bdevs)
	claimed=$(echo $status |  jq "map(select(.name == \"$1\")) | .[0].claimed")
	if [[ $claimed == true ]]; then
		return 0
	else
		return 1
	fi
}

$rootdir/app/iscsi_tgt/iscsi_tgt -c "$curdir/environment.conf" &
spdk_pid=$!

trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT

sleep 1

$rpc_py construct_cas_bdev PartCache wt Malloc0 NonExisting

if ! check_claimed Malloc0; then
	>&2 echo "Base device expected to be claimed now"
	return 1
fi

$rpc_py delete_cas_bdev PartCache
if check_claimed Malloc0; then
	>&2 echo "Base device is not expected to be claimed now"
	return 1
fi

$rpc_py construct_cas_bdev FullCache wt Malloc2 Malloc3

if ! (check_claimed Malloc2 && check_claimed Malloc3); then
	>&2 echo "Base devices expected to be claimed now"
	return 1
fi

$rpc_py delete_cas_bdev FullCache
if check_claimed Malloc2 && check_claimed Malloc3; then
	>&2 echo "Base devices are not expected to be claimed now"
	return 1
fi

$rpc_py construct_cas_bdev HotCache wt Malloc2 Malloc3

if ! (check_claimed Malloc2 && check_claimed Malloc3); then
	>&2 echo "Base devices expected to be claimed now"
	return 1
fi

$rpc_py delete_malloc_bdev Malloc2

if check_claimed Malloc2; then
	>&2 echo "Base device is not expected to be claimed now"
	return 1
fi

status=$($rpc_py get_bdevs)
gone=$(echo $status | jq 'map(select(.name == "HotCache")) == []')
if [[ $gone == false ]]; then
	>&2 echo "CAS bdev is expected to unregister"
	return 1
fi

# check if shutdown of running CAS bdev is ok
$rpc_py construct_cas_bdev FullCache wt Malloc0 Malloc1
$rpc_py construct_cas_bdev PartCache wt NonExisting Malloc3

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
