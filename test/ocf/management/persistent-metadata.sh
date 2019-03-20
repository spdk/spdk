#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

function bdev_check_claimed()
{
       if $($rpc_py get_bdevs -b "$@" | jq '.[0].claimed'); then
               return 0;
       else
               return 1;
       fi
}

function bdev_check_created()
{
       if $($rpc_py get_bdevs -b "$@"); then
               return 0;
       else
               return 1;
       fi
}

$rootdir/app/iscsi_tgt/iscsi_tgt &
spdk_pid=$!


# Create ocf on persistent storage

trunc -s 128M ./aio0
trunc -s 128M ./aio1
trunc -s 128M ./aio2

$rpc_py construct_aio_bdev ./aio0 AIO0 4096
$rpc_py construct_aio_bdev ./aio1 AIO1 4096
$rpc_py construct_aio_bdev ./aio2 AIO2 4096

$rpc_py construct_ocf_bdev ocf0 wt AIO0 AIO1
$rpc_py construct_ocf_bdev ocf1 wt AIO0 AIO2

if ! (bdev_check_claimed AIO0 && bdev_check_claimed AIO1); then
	>&2 echo "Base devices expected to be claimed now"
	exit 1
fi


trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
$rootdir/app/iscsi_tgt/iscsi_tgt &
spdk_pid=$!

trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

$rpc_py construct_aio_bdev ./aio0 AIO0 4096
$rpc_py construct_aio_bdev ./aio1 AIO1 4096
$rpc_py construct_aio_bdev ./aio2 AIO2 4096

# OCF should be loaded now as well

if ! (bdev_check_claimed AIO0 && bdev_check_claimed AIO1 && bdev_check_claimed AIO2); then
	>&2 echo "Base devices expected to be claimed now"
	exit 1
fi

if ! (bdev_check_created ocf0 && bdev_check_created ocf1); then
	>&2 echo "OCF devices not restored properly"
	exit 1
fi


trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm aio0 aio1 aio2
