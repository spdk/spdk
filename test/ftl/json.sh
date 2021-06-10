#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

device=$1

json_kill() {
	killprocess $svcpid
}

trap "json_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

# Create new bdev from json configuration
$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
bdev_create_zone nvme0n1
$rootdir/scripts/gen_ftl.sh -n ftl0 -d "$ZONE_DEV" | $rpc_py load_subsystem_config

waitforbdev ftl0
uuid=$($rpc_py bdev_get_bdevs | jq -r ".[] | select(.name==\"ftl0\").uuid")

$rpc_py bdev_ftl_delete -b ftl0

# Restore bdev from json configuration
$rootdir/scripts/gen_ftl.sh -n ftl0 -d "$ZONE_DEV" -u $uuid | $rpc_py load_subsystem_config
$rpc_py bdev_ftl_delete -b ftl0
$rpc_py bdev_nvme_detach_controller nvme0

trap - SIGINT SIGTERM EXIT
json_kill
