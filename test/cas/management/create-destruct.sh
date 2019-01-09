#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

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

function partial()
{
	sleep 1

	$rpc_py construct_cas_bdev TestCache wt Malloc0 NonExisting

	if ! check_claimed Malloc0; then
		>&2 echo "Base device expected to be claimed now"
		return 1
	fi

	$rpc_py delete_cas_bdev TestCache
	if check_claimed Malloc0; then
		>&2 echo "Base device is not expected to be claimed now"
		return 1
	fi

	# check if shutdown of partial CAS bdev is ok
	$rpc_py construct_cas_bdev TestCache wt NonExisting Malloc0
}

function full()
{
	sleep 1

	$rpc_py construct_cas_bdev TestCache wt Malloc0 Malloc1

	if ! (check_claimed Malloc0 && check_claimed Malloc1); then
		>&2 echo "Base devices expected to be claimed now"
		return 1
	fi


	$rpc_py delete_cas_bdev TestCache
	if check_claimed Malloc0 && check_claimed Malloc1; then
		>&2 echo "Base devices are not expected to be claimed now"
		return 1
	fi

	# check if shutdown of running CAS bdev is ok
	$rpc_py construct_cas_bdev TestCache wt Malloc0 Malloc1
}

function hotremove()
{
	sleep 1

	$rpc_py construct_cas_bdev TestCache wt Malloc0 Malloc1

	if ! (check_claimed Malloc0 && check_claimed Malloc1); then
		>&2 echo "Base devices expected to be claimed now"
		return 1
	fi

	$rpc_py delete_malloc_bdev Malloc0

	if check_claimed Malloc1; then
		>&2 echo "Base device is not expected to be claimed now"
		return 1
	fi

	status=$($rpc_py get_bdevs)
	gone=$(echo $status | jq 'map(select(.name == "TestCache")) == []')
	if [[ $gone == false ]]; then
		>&2 echo "CAS bdev is expected to unregister"
		return 1
	fi
}

with_spdk partial
with_spdk full
with_spdk hotremove
