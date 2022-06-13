#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

device=$1
cache=$2
zone_size=$[262144]
write_unit_size=16
timeout=240

json_kill() {
	killprocess $svcpid
}

trap "json_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

# Create new bdev from json configuration
$rpc_py bdev_nvme_attach_controller -b nvme1 -a $cache -t pcie
split_bdev=$($rootdir/scripts/rpc.py bdev_split_create nvme0n1 -s $((1024*101))  1)
$rootdir/scripts/gen_ftl.sh -n ftl0 -d $split_bdev -c nvme1n1 | $rpc_py -t $timeout load_subsystem_config
$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie

waitforbdev ftl0
uuid=$($rpc_py bdev_get_bdevs | jq -r ".[] | select(.name==\"ftl0\").uuid")

$rpc_py bdev_ftl_delete -b ftl0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -n ftl0 -d $split_bdev -c nvme1n1 -u $uuid | $rpc_py load_subsystem_config
$rpc_py bdev_ftl_delete -b ftl0
$rpc_py bdev_nvme_detach_controller nvme0

trap - SIGINT SIGTERM EXIT
json_kill
