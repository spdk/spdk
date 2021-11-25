#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# Positive test for lvol store and lvol bdev rename.
function test_rename_positive() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	bdev_name=("lvol_test"{0..3})
	bdev_aliases=("lvs_test/lvol_test"{0..3})

	# Calculate size and create two lvol bdevs on top
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 4)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	# Create 4 lvol bdevs on top of previously created lvol store
	bdev_uuids=()
	for i in "${!bdev_name[@]}"; do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" "${bdev_name[i]}" "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b $lvol_uuid)
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases[i]}'"]')" ]
		bdev_uuids+=("$lvol_uuid")
	done

	# Rename lvol store and check if lvol store name and
	# lvol bdev aliases were updated properly
	new_lvs_name="lvs_new"
	bdev_aliases=("$new_lvs_name/lvol_test"{0..3})

	rpc_cmd bdev_lvol_rename_lvstore lvs_test "$new_lvs_name"

	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid")

	# verify it's there
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "$new_lvs_name" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name" ]

	# verify some of its parameters
	cluster_size=$(jq -r '.[0].cluster_size' <<< "$lvs")
	[ "$cluster_size" = "$LVS_DEFAULT_CLUSTER_SIZE" ]
	total_clusters=$(jq -r '.[0].total_data_clusters' <<< "$lvs")
	[ "$((total_clusters * cluster_size))" = "$LVS_DEFAULT_CAPACITY" ]

	for i in "${!bdev_uuids[@]}"; do
		lvol=$(rpc_cmd bdev_get_bdevs -b "${bdev_uuids[i]}")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq -r '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases[i]}'"]')" ]
	done

	# Now try to rename the bdevs using their uuid as "old_name"
	# Verify that all bdev names were successfully updated
	bdev_names=("lbd_new"{0..3})
	new_bdev_aliases=()
	for bdev_name in "${bdev_names[@]}"; do
		new_bdev_aliases+=("$new_lvs_name/$bdev_name")
	done
	for i in "${!bdev_names[@]}"; do
		rpc_cmd bdev_lvol_rename "${bdev_aliases[i]}" "${bdev_names[i]}"
		lvol=$(rpc_cmd bdev_get_bdevs -b "${bdev_uuids[i]}")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq -r '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${new_bdev_aliases[i]}'"]')" ]
	done

	# Clean up
	for bdev in "${new_bdev_aliases[@]}"; do
		rpc_cmd bdev_lvol_delete "$bdev"
	done
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Negative test case for lvol store rename.
# Check that error is returned when trying to rename not existing lvol store.
# Check that error is returned when trying to rename to a name which is already
# used by another lvol store.
function test_rename_lvs_negative() {
	# Call bdev_lvol_rename_lvstore with name pointing to not existing lvol store
	rpc_cmd bdev_lvol_rename_lvstore NOTEXIST WHATEVER && false

	# Construct two malloc bdevs
	malloc_name1=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	malloc_name2=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)

	# Create lvol store on each malloc bdev
	lvs_uuid1=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name1" lvs_test1)
	lvs_uuid2=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name2" lvs_test2)

	# Create lists with lvol bdev names and aliases for later use
	bdev_names_1=("lvol_test_1_"{0..3})
	bdev_names_2=("lvol_test_2_"{0..3})
	bdev_aliases_1=("lvs_test1/lvol_test_1_"{0..3})
	bdev_aliases_2=("lvs_test2/lvol_test_2_"{0..3})

	# Calculate size and create two lvol bdevs on top
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 4)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	# # Create 4 lvol bdevs on top of each lvol store
	bdev_uuids_1=()
	bdev_uuids_2=()
	for i in "${!bdev_names_1[@]}"; do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "${bdev_names_1[i]}" "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid1" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases_1[i]}'"]')" ]
		bdev_uuids_1+=("$lvol_uuid")

		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid2" "${bdev_names_2[i]}" "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid2" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases_2[i]}'"]')" ]
		bdev_uuids_2+=("$lvol_uuid")
	done

	# Call bdev_lvol_rename_lvstore on first lvol store and try to change its name to
	# the same name as used by second lvol store
	rpc_cmd bdev_lvol_rename_lvstore lvs_test1 lvs_test2 && false

	# Verify that names of lvol stores and lvol bdevs did not change
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid1")
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid1" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "lvs_test1" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name1" ]
	[ "$(jq -r '.[0].cluster_size' <<< "$lvs")" = "$LVS_DEFAULT_CLUSTER_SIZE" ]
	lvs=$(rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid2")
	[ "$(jq -r '.[0].uuid' <<< "$lvs")" = "$lvs_uuid2" ]
	[ "$(jq -r '.[0].name' <<< "$lvs")" = "lvs_test2" ]
	[ "$(jq -r '.[0].base_bdev' <<< "$lvs")" = "$malloc_name2" ]
	[ "$(jq -r '.[0].cluster_size' <<< "$lvs")" = "$LVS_DEFAULT_CLUSTER_SIZE" ]

	for i in "${!bdev_uuids_1[@]}"; do
		lvol=$(rpc_cmd bdev_get_bdevs -b "${bdev_uuids_1[i]}")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid1" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases_1[i]}'"]')" ]

		lvol=$(rpc_cmd bdev_get_bdevs -b "${bdev_uuids_2[i]}")
		[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid2" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
		[ "$(jq '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["'${bdev_aliases_2[i]}'"]')" ]
	done

	# Clean up
	for bdev in "${bdev_aliases_1[@]}" "${bdev_aliases_2[@]}"; do
		rpc_cmd bdev_lvol_delete "$bdev"
	done
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid1"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid2"
	rpc_cmd bdev_malloc_delete "$malloc_name1"
	rpc_cmd bdev_malloc_delete "$malloc_name2"
	check_leftover_devices
}

# Negative test case for lvol bdev rename.
# Check that error is returned when trying to rename not existing lvol bdev
# Check that error is returned when trying to rename to a name which is already
# used by another lvol bdev.
function test_lvol_rename_negative() {
	# Call bdev_lvol_rename with name pointing to not existing lvol bdev
	rpc_cmd bdev_lvol_rename NOTEXIST WHATEVER && false

	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate lvol bdev size
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 2)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	# Create two lvol bdevs on top of previously created lvol store
	lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test1 "$lvol_size_mb")
	lvol_uuid2=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test2 "$lvol_size_mb")

	# Call bdev_lvol_rename on first lvol bdev and try to change its name to
	# the same name as used by second lvol bdev
	rpc_cmd bdev_lvol_rename lvol_test1 lvol_test2 && false

	# Verify that lvol bdev still have the same names as before
	lvol=$(rpc_cmd bdev_get_bdevs -b $lvol_uuid1)
	[ "$(jq -r '.[0].driver_specific.lvol.lvol_store_uuid' <<< "$lvol")" = "$lvs_uuid" ]
	[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$MALLOC_BS" ]
	[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / MALLOC_BS))" ]
	[ "$(jq -r '.[0].aliases|sort' <<< "$lvol")" = "$(jq '.|sort' <<< '["lvs_test/lvol_test1"]')" ]

	rpc_cmd bdev_lvol_delete lvs_test/lvol_test1
	rpc_cmd bdev_lvol_delete lvs_test/lvol_test2
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_rename_positive" test_rename_positive
run_test "test_rename_lvs_negative" test_rename_lvs_negative
run_test "test_lvol_rename_negative" test_lvol_rename_negative

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
