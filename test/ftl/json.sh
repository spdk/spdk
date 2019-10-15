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

$rootdir/app/spdk_tgt/spdk_tgt & svcpid=$!
waitforlisten $svcpid

# Create new bdev from json configuration
$rootdir/scripts/gen_ftl.sh -n ftl0 -d nvme0n1 | $rpc_py load_subsystem_config
$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1

waitforbdev ftl0
uuid=$($rpc_py bdev_get_bdevs | jq -r ".[] | select(.name==\"ftl0\").uuid")

$rpc_py bdev_ftl_delete -b nvme0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n nvme0 -u $uuid | $rpc_py load_subsystem_config
$rpc_py bdev_ftl_delete -b nvme0
# Create new bdev from RPC
$rpc_py bdev_ftl_create -b nvme1 -a $device
$rpc_py bdev_ftl_delete -b nvme1

# TODO: add negative test cases

trap - SIGINT SIGTERM EXIT
json_kill
