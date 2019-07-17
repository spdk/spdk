#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

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

	# resize the lvol to four times its original size, use its name instead of uuid
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd resize_lvol_bdev lvs_test/lvol_test "$lvol_size_mb"
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

# resize an lvol a few times
function test_resize_lvol_with_io_traffic() {
	# create an lvol store
	malloc_name=$(rpc_cmd construct_malloc_bdev $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd construct_lvol_store "$malloc_name" lvs_test)

	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
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

	# prepare to do some I/O
	modprobe nbd
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	trap "nbd_stop_disks '$DEFAULT_RPC_ADDR' /dev/nbd0; exit 1" SIGINT SIGTERM EXIT

	# write to the entire lvol
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE ))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count

	# writing beyond lvol size should fail
	offset=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE + 1 ))
	! dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" seek=$offset count=1

	# resize the lvol to twice its original size
	lvol_size_mb=$(( lvol_size_mb * 2 ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	rpc_cmd resize_lvol_bdev "$lvol_uuid" "$lvol_size_mb"
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]

	# writing beyond the original lvol size should now succeed, we need
	# to restart NBD though as it may still use the old, cached size
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" seek=$offset count=1

	# lvol can't be downsized if they have any open descriptors, so close them now
	trap - SIGINT SIGTERM EXIT
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# resize lvol down to a single cluster
	rpc_cmd resize_lvol_bdev "$lvol_uuid" "$LVS_DEFAULT_CLUSTER_SIZE_MB"
	lvol=$(rpc_cmd get_bdevs -b "$lvol_uuid")
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( LVS_DEFAULT_CLUSTER_SIZE / MALLOC_BS ))" ]

	# make sure we can't write beyond the first cluster
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	trap "nbd_stop_disks '$DEFAULT_RPC_ADDR' /dev/nbd0; exit 1" SIGINT SIGTERM EXIT
	! dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" seek=1 count=1

	# clean up
	trap - SIGINT SIGTERM EXIT
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
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
run_test test_resize_lvol_with_io_traffic

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
