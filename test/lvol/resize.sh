#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# resize an lvol a few times
function test_resize_lvol() {
	# create an lvol store
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# calculate lvol size
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 4 )) )
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	# create an lvol on top
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
	[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test/lvol_test" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to twice its original size
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd bdev_lvol_resize "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to four times its original size
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd bdev_lvol_resize "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# resize the lvol to 0 using lvol bdev alias
	lvol_size_mb=0
	lvol_size=0
	rpc_cmd bdev_lvol_resize "lvs_test/lvol_test" "$lvol_size_mb"
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

# negative test for resizing a logical volume
# call bdev_lvol_resize with logical volume which does not exist in configuration
# call bdev_lvol_resize with size argument bigger than size of base bdev
function test_resize_lvol_negative() {
	# create an lvol store
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# create an lvol on top
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")

	# try to resize another, inexistent lvol
	dummy_uuid="00000000-0000-0000-0000-000000000000"
	rpc_cmd bdev_lvol_resize "$dummy_uuid" 0 && false
	# just make sure the size of the real lvol did not change
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]

	# try to resize an lvol to a size bigger than lvs
	rpc_cmd bdev_lvol_resize "$lvol_uuid" "$MALLOC_SIZE_MB" && false
	# just make sure the size of the real lvol did not change
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CAPACITY / MALLOC_BS ))" ]

	# clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_resize_lvol" test_resize_lvol
run_test "test_resize_lvol_negative" test_resize_lvol_negative

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
