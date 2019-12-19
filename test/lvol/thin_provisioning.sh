#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

# Check if free clusters number on lvol store decreases
# if we write to created thin provisioned lvol bdev
function test_thin_lvol_check_space() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_start="$(jq -r '.[0].free_clusters' <<< "$lvs")"

        # Create thin provision lvol bdev with size equals to lvol store space
        lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB ))
        # Round down lvol size to the nearest cluster size boundary
        lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
        lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
        lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)

	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_create_lvol="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $free_clusters_start == $free_clusters_create_lvol ]

	# Write data (lvs cluster size) to created lvol bdev starting from offset 0.
	size=$LVS_DEFAULT_CLUSTER_SIZE
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $size "write" "0xcc"
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_first_fio="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $(( free_clusters_first_fio + 1 )) == $free_clusters_start ]

	# Write data (lvs cluster size) to lvol bdev with offset set to one and half of cluster size
	offset=$(( LVS_DEFAULT_CLUSTER_SIZE * 3 / 2 ))
	size=$LVS_DEFAULT_CLUSTER_SIZE
	run_fio_test /dev/nbd0 $offset $size "write" "0xcc"
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_second_fio="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $(( free_clusters_second_fio + 3 )) == $free_clusters_start ]

	# write data to lvol bdev to the end of its size
	size=$(( LVS_DEFAULT_CLUSTER_SIZE * free_clusters_first_fio ))
	offset=$(( 3 * LVS_DEFAULT_CLUSTER_SIZE ))
	run_fio_test /dev/nbd0 $offset $size "write" "0xcc"
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	# Check that lvol store free clusters number equals to 0
	free_clusters_third_fio="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $(( free_clusters_third_fio )) == 0 ]

	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	! rpc_cmd bdev_get_bdevs -b "$lvol_uuid"
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_end="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $(( free_clusters_end )) == $free_clusters_start ]

	# Clean up
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

# Check if we can create thin provisioned bdev on empty lvol store
# and check if we can read from this device and it returns zeroes.
function test_thin_lvol_check_zeroes() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_start="$(jq -r '.[0].free_clusters' <<< "$lvs")"

	# Create thick and thin provisioned lvol bdevs with size equals to lvol store space
	lbd_name0=lvol_test0
	lbd_name1=lvol_test1
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB ))
	# Round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	lvol_uuid0=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" $lbd_name0 "$lvol_size_mb")
	lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" $lbd_name1 "$lvol_size_mb" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid0" /dev/nbd0
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" /dev/nbd1

	# Fill the whole thick provisioned lvol bdev
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"

	# Perform read operations on thin provisioned lvol bdev
	# and check if they return zeroes
	run_fio_test /dev/nbd1 0 $lvol_size "read" "0x00"

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid1"
	rpc_cmd bdev_lvol_delete "$lvol_uuid0"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

# Check if data written to thin provisioned lvol bdev
# were properly written (fio test with verification)
function test_thin_lvol_check_integrity() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create thin provisioned lvol bdev with size equals to lvol store space
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB ))
	# Round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

# Check thin provisioned bdev resize
function test_thin_lvol_resize() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Construct thin provisioned lvol bdevs on created lvol store
	# with size equal to 50% of lvol store
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
	# Round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)

	# Fill all free space of lvol bdev with data
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 $lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Save number of free clusters for lvs
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_start="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	# Resize bdev to full size of lvs
	lvol_size_full_mb=$(( LVS_DEFAULT_CAPACITY_MB ))
        lvol_size_full_mb=$(( lvol_size_full_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size_full=$(( lvol_size_full_mb * 1024 * 1024 ))
	rpc_cmd bdev_lvol_resize $lvol_uuid $lvol_size_full_mb

	# Check if bdev size changed (total_data_clusters*cluster_size
	# equals to num_blocks*block_size)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
        [ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	echo "asdasd $lvol_size_full"
        [ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = $(( lvol_size_full / MALLOC_BS )) ]

	# Check if free_clusters on lvs remain unaffected
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_resize="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $free_clusters_start == $free_clusters_resize ]

	# Perform write operation with verification
	# to newly created free space of lvol bdev
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
        run_fio_test /dev/nbd0 0 $lvol_size_full "write" "0xcc"
        nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Check if free clusters on lvs equals to zero
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_start="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $free_clusters_start == 0 ]

	# Resize bdev to 25% of lvs and check if it ended with success
	lvol_size_quarter_mb=$(( LVS_DEFAULT_CAPACITY_MB / 4 ))
        lvol_size_quarter_mb=$(( lvol_size_quarter_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	rpc_cmd bdev_lvol_resize $lvol_uuid $lvol_size_quarter_mb

	# Check free clusters on lvs
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_resize_quarter="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	free_clusters_expected=$(( (lvol_size_full_mb - lvol_size_quarter_mb) / LVS_DEFAULT_CLUSTER_SIZE_MB ))
	[ $free_clusters_resize_quarter == $free_clusters_expected ]

	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_lvol_test test_thin_lvol_check_space
run_lvol_test test_thin_lvol_check_zeroes
run_lvol_test test_thin_lvol_check_integrity
run_lvol_test test_thin_lvol_resize

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
