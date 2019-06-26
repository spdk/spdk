#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# create empty lvol store and verify its parameters
function test_construct_lvs() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)
	lvs=$(rpc_cmd get_lvol_stores -u "$lvs_uuid")

	# verify it's there
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "lvs_test" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name" ]

	# verify some of its parameters
	cluster_size=$(jq -r '.[0].cluster_size' <<< "$lvs")
	[ "$cluster_size" = "$LVS_DEFAULT_CLUSTER_SIZE" ]
	total_clusters=$(jq -r '.[0].total_data_clusters' <<< "$lvs")
	[ "$(jq -r '.[0].free_clusters' <<< "$lvs")" = "$total_clusters" ]
	[ "$(( total_clusters * cluster_size ))" = "$LVS_DEFAULT_CAPACITY" ]

	# remove it and verify it's gone
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

# create lvs + lvol on top, verify lvol's parameters
function test_construct_lvol() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# create an lvol on top
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")

	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]

	# clean up
	rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test test_construct_lvs
run_test test_construct_lvol

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
