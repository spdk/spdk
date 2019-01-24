#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

$rootdir/app/iscsi_tgt/iscsi_tgt &
spdk_pid=$!

trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

for i in $(seq 0 4); do
	$rpc_py construct_malloc_bdev 101 512 -b Malloc${i}
done

$rpc_py construct_ocf_bdev PartCache wt Malloc0 NonExisting

if ! bdev_check_claimed Malloc0; then
	>&2 echo "Base device expected to be claimed now"
	exit 1
fi

$rpc_py delete_ocf_bdev PartCache
if bdev_check_claimed Malloc0; then
	>&2 echo "Base device is not expected to be claimed now"
	exit 1
fi

$rpc_py construct_ocf_bdev FullCache wt Malloc2 Malloc3

if ! (bdev_check_claimed Malloc2 && bdev_check_claimed Malloc3); then
	>&2 echo "Base devices expected to be claimed now"
	exit 1
fi

$rpc_py delete_ocf_bdev FullCache
if bdev_check_claimed Malloc2 && bdev_check_claimed Malloc3; then
	>&2 echo "Base devices are not expected to be claimed now"
	exit 1
fi

# check if shutdown of running CAS bdev is ok
$rpc_py construct_ocf_bdev FullCache wt Malloc0 Malloc1
$rpc_py construct_ocf_bdev PartCache wt NonExisting Malloc3

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
