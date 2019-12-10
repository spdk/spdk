#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# Positive test for lvol store and lvol bdev rename.
function test_rename_positive() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	bdev_name=()
	for i in $(seq 0 3); do
		bdev_name+=("lvol_test$i")
	done
	bdev_aliases=()
	for i in $(seq 0 3); do
		bdev_aliases+=("lvs_test/lvol_test$i")
	done

	# Calculate size and create two lvol bdevs on top
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 4 )) )
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	# Create 4 lvol bdevs on top of previously created lvol store
	bdev_uuids=()
	for i in $(seq 0 3); do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" ${bdev_name[$i]} "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b $lvol_uuid)
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases[$i]}'"]')" ]
		bdev_uuids+=($lvol_uuid)
	done

	# Rename lvol store and check if lvol store name and
	# lvol bdev aliases were updated properly
	new_lvs_name="lvs_new"
	bdev_aliases=()
	for i in $(seq 0 3); do
		bdev_aliases+=("$new_lvs_name/lvol_test$i")
	done

	rpc_cmd bdev_lvol_rename_lvstore lvs_test $new_lvs_name

	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")

	# verify it's there
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "$new_lvs_name" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name" ]

	# verify some of its parameters
	cluster_size=$(jq -r '.[0].cluster_size' <<< "$lvs")
	[ "$cluster_size" = "$LVS_DEFAULT_CLUSTER_SIZE" ]
	total_clusters=$(jq -r '.[0].total_data_clusters' <<< "$lvs")
	[ "$(( total_clusters * cluster_size ))" = "$LVS_DEFAULT_CAPACITY" ]

	for i in $(seq 0 3); do
		lvol=$(rpc_cmd bdev_get_bdevs -b ${bdev_uuids[$i]})
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]
		[ "$(jq -r '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases[$i]}'"]')" ]
	done

	# Now try to rename the bdevs using their uuid as "old_name"
	# Verify that all bdev names were successfully updated
	bdev_names=()
	new_bdev_aliases=()
	for i in $(seq 0 3); do
		bdev_names+=("lbd_new$i")
		new_bdev_aliases+=("$new_lvs_name/${bdev_names[$i]}")
	done
	for i in $(seq 0 3); do
		rpc_cmd bdev_lvol_rename ${bdev_aliases[$i]} ${bdev_names[$i]}
		lvol=$(rpc_cmd bdev_get_bdevs -b ${bdev_uuids[$i]})
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / MALLOC_BS ))" ]
		[ "$(jq -r '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${new_bdev_aliases[$i]}'"]')" ]
	done

	# Clean up
	for i in $(seq 0 3); do
		rpc_cmd bdev_lvol_delete "${new_bdev_aliases[$i]}"
	done
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_rename_positive" test_rename_positive

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
