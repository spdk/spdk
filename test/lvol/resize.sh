#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# resize an lvol a few times
function test_resize_lvol() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 4 ))
	# round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	# create an lvol on top
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to twice its original size
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd resize_lvol_bdev "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to four times its original size
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd resize_lvol_bdev "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to 0
	lvol_size_mb=0
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd resize_lvol_bdev "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# clean up
	rpc_cmd destroy_lvol_bdev "$lvol_uuid"
	! rpc_cmd get_bdevs -b "$lvol_uuid"
	rpc_cmd destroy_lvol_store -u "$lvs_uuid"
	! rpc_cmd get_lvol_stores -u "$lvs_uuid"
	rpc_cmd delete_malloc_bdev "$malloc_name"
}

function test_resize_lvol_negative() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	# create an lvol on top
	lvol_uuid=$(rpc_cmd construct_lvol_bdev -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")

	# try to resize another, inexistent lvol
	dummy_uuid="00000000-0000-0000-0000-000000000000"
	! rpc_cmd resize_lvol_bdev "$dummy_uuid" 0
	# just make the size of the real lvol did not change
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]

	# try to resize an lvol to a size bigger than lvs
	! rpc_cmd resize_lvol_bdev "$lvol_uuid" "$MALLOC_SIZE_MB"
	# just make the size of the real lvol did not change
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
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

run_test test_resize_lvol
run_test test_resize_lvol_negative

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
