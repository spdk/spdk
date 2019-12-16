#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

# Check if number of free clusters on lvol store decreases
# if we write to created thin provisioned lvol bdev
function test_thin_lvol_check_space() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_start="$(jq -r '.[0].free_clusters' <<< "$lvs")"

	# Create thin provision lvol bdev with size equals to lvol store space
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB )) )
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
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")
	free_clusters_end="$(jq -r '.[0].free_clusters' <<< "$lvs")"
	[ $(( free_clusters_end )) == $free_clusters_start ]

	# Clean up
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_thin_lvol_check_space" test_thin_lvol_check_space

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
