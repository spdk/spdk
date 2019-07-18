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
	! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"
	lvolstores=$(rpc_cmd bdev_lvol_get_lvstores)
	[ "$(jq length <<< "$lvolstores")" == "0" ]

	# make sure we can't destroy the lvs again
	! rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"

	# make sure the lvol is also gone
	! rpc_cmd bdev_get_bdevs -b "$lvol_uuid"
	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

# destroy lvs with 4 lvols on top
function test_hotremove_lvol_store_multiple_lvols() {
	# create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	lvol_size_mb=$(( (MALLOC_SIZE_MB- LVS_DEFAULT_CLUSTER_SIZE_MB) / 4 ))
	# round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))

	# create 4 lvols
	for i in $(seq 1 4); do
		rpc_cmd bdev_lvol_create -u "$lvs_uuid" "lvol_test${i}" "$lvol_size_mb"
	done

	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "4" ]

	# remove lvs (with 4 lvols open)
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"

	# make sure all lvols are gone
	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_lvol_test test_hotremove_lvol_store
run_lvol_test test_hotremove_lvol_store_multiple_lvols

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
