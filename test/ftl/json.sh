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

ocssd_bdev=$($rpc_py bdev_ocssd_attach_controller -b nvme0 -a $device)
# Create new bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n ftl0 -z $ocssd_bdev | $rpc_py load_subsystem_config

uuid=$($rpc_py get_bdevs | jq -r '.[1].uuid')

$rpc_py delete_ftl_bdev -b ftl0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -j -a $device -n ftl0 -z $ocssd_bdev -u $uuid | $rpc_py load_subsystem_config
$rpc_py delete_ftl_bdev -b ftl0

trap - SIGINT SIGTERM EXIT
json_kill
