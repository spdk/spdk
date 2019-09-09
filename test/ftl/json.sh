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

$rpc_py construct_nvme_bdev -b nvme0 -a $device -m ocssd -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1
# Create new bdev from json configuration
$rootdir/scripts/gen_ftl.sh -n ftl0 -d nvme0n1 | $rpc_py load_subsystem_config

uuid=$($rpc_py get_bdevs | jq -r '.[1].uuid')

$rpc_py delete_ftl_bdev -b ftl0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -n ftl0 -d nvme0n1 -u $uuid | $rpc_py load_subsystem_config
$rpc_py delete_ftl_bdev -b ftl0

trap - SIGINT SIGTERM EXIT
json_kill
