#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# create an lvol on lvs, then remove the lvs
function test_hotremove_lvol_store() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")

	# remove lvs (with one lvol open)
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	lvolstores=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$lvolstores")" == "0" ]

	# make sure we can't destroy the lvs again
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid" && false

	# make sure the lvol is also gone
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_hotremove_lvol_store" test_hotremove_lvol_store

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
