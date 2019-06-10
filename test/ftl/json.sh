#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py


device=$1
FTL_BDEV_CONF=$testdir/config/ftl.json

json_kill() {
	killprocess $svcpid
}

trap "json_kill; exit 1" SIGINT SIGTERM EXIT

$rootdir/test/app/bdev_svc/bdev_svc & svcpid=$!
waitforlisten $svcpid

# Create new bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n nvme0 -l 0-3 | $rpc_py load_subsystem_config

uuid=$($rpc_py get_bdevs | jq -r '.[0].uuid')

$rpc_py delete_ftl_bdev -b nvme0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n nvme0 -l 0-3 -u $uuid | $rpc_py load_subsystem_config
# Create new bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n nvme1 -l 4-5 | $rpc_py load_subsystem_config
# Create new bdev from RPC
$rpc_py construct_ftl_bdev -b nvme2 -a $device -l 7-7

$rpc_py delete_ftl_bdev -b nvme2
$rpc_py delete_ftl_bdev -b nvme0
$rpc_py delete_ftl_bdev -b nvme1

# TODO: add negative test cases

trap - SIGINT SIGTERM EXIT
json_kill
