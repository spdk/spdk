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

# destroy lvs with 4 lvols on top
function test_hotremove_lvol_store_multiple_lvols() {
	# create lvs
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# calculate lvol size
	lvol_size_mb=$(round_down $(((MALLOC_SIZE_MB - LVS_DEFAULT_CLUSTER_SIZE_MB) / 4)))

	# create 4 lvols
	for i in $(seq 1 4); do
		rpc_cmd bdev_lvol_create -u "$lvs_uuid" "lvol_test${i}" "$lvol_size_mb"
	done

	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "4" ]

	# remove lvs (with 4 lvols open)
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false

	# make sure all lvols are gone
	lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$lvols")" == "0" ]

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# create an lvs on malloc, then remove just the malloc
function test_hotremove_lvol_store_base() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
	# make sure the lvs is gone
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	# make sure we can't delete the lvs again
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid" && false
	check_leftover_devices
}

# create an lvs on malloc, then an lvol, then remove just the malloc
function test_hotremove_lvol_store_base_with_lvols() {
	# create lvs + lvol on top
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$LVS_DEFAULT_CAPACITY_MB")

	rpc_cmd bdev_get_bdevs -b "$lvol_uuid"

	# clean up
	rpc_cmd bdev_malloc_delete "$malloc_name"
	# make sure the lvol is gone
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	# make sure the lvs is gone as well
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false

	# make sure we can't delete the lvs again
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid" && false
	check_leftover_devices
}

function test_bdev_lvol_delete_lvstore_with_clones() {
	local snapshot_name1=snapshot1 snapshot_uuid1
	local snapshot_name2=snapshot2 snapshot_uuid2
	local clone_name=clone clone_uuid
	local lbd_name=lbd_test

	local bdev_uuid
	local lvstore_name=lvs_name lvstore_uuid
	local malloc_dev

	malloc_dev=$(rpc_cmd bdev_malloc_create 256 "$MALLOC_BS")
	lvstore_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_dev" "$lvstore_name")

	get_lvs_jq bdev_lvol_get_lvstores -u "$lvstore_uuid"
	[[ ${jq_out["uuid"]} == "$lvstore_uuid" ]]
	[[ ${jq_out["name"]} == "$lvstore_name" ]]
	[[ ${jq_out["base_bdev"]} == "$malloc_dev" ]]

	size=$((jq_out["free_clusters"] * jq_out["cluster_size"] / 4 / 1024 ** 2))

	bdev_uuid=$(rpc_cmd bdev_lvol_create -u "$lvstore_uuid" "$lbd_name" "$size")

	get_bdev_jq bdev_get_bdevs -b "$bdev_uuid"

	snapshot_uuid1=$(rpc_cmd bdev_lvol_snapshot "${jq_out["name"]}" "$snapshot_name1")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$snapshot_name1"
	[[ ${jq_out["name"]} == "$snapshot_uuid1" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$snapshot_name1" ]]

	clone_uuid=$(rpc_cmd bdev_lvol_clone "$lvstore_name/$snapshot_name1" "$clone_name")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$clone_name"
	[[ ${jq_out["name"]} == "$clone_uuid" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$clone_name" ]]

	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot "${jq_out["name"]}" "$snapshot_name2")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$snapshot_name2"
	[[ ${jq_out["name"]} == "$snapshot_uuid2" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$snapshot_name2" ]]

	rpc_cmd bdev_lvol_delete "$snapshot_uuid1" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvstore_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_dev"

	check_leftover_devices
}

# Test for unregistering the lvol bdevs. Removing malloc bdev under an lvol
# store triggers unregister of all lvol bdevs. Verify it with clones present.
function test_unregister_lvol_bdev() {
	local snapshot_name1=snapshot1 snapshot_uuid1
	local snapshot_name2=snapshot2 snapshot_uuid2
	local clone_name=clone clone_uuid
	local lbd_name=lbd_test

	local bdev_uuid
	local lvstore_name=lvs_name lvstore_uuid
	local malloc_dev

	malloc_dev=$(rpc_cmd bdev_malloc_create 256 "$MALLOC_BS")
	lvstore_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_dev" "$lvstore_name")

	get_lvs_jq bdev_lvol_get_lvstores -u "$lvstore_uuid"
	[[ ${jq_out["uuid"]} == "$lvstore_uuid" ]]
	[[ ${jq_out["name"]} == "$lvstore_name" ]]
	[[ ${jq_out["base_bdev"]} == "$malloc_dev" ]]

	size=$((jq_out["free_clusters"] * jq_out["cluster_size"] / 4 / 1024 ** 2))

	bdev_uuid=$(rpc_cmd bdev_lvol_create -u "$lvstore_uuid" "$lbd_name" "$size")

	get_bdev_jq bdev_get_bdevs -b "$bdev_uuid"

	snapshot_uuid1=$(rpc_cmd bdev_lvol_snapshot "${jq_out["name"]}" "$snapshot_name1")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$snapshot_name1"
	[[ ${jq_out["name"]} == "$snapshot_uuid1" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$snapshot_name1" ]]

	clone_uuid=$(rpc_cmd bdev_lvol_clone "$lvstore_name/$snapshot_name1" "$clone_name")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$clone_name"
	[[ ${jq_out["name"]} == "$clone_uuid" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$clone_name" ]]

	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot "${jq_out["name"]}" "$snapshot_name2")

	get_bdev_jq bdev_get_bdevs -b "$lvstore_name/$snapshot_name2"
	[[ ${jq_out["name"]} == "$snapshot_uuid2" ]]
	[[ ${jq_out["product_name"]} == "Logical Volume" ]]
	[[ ${jq_out["aliases[0]"]} == "$lvstore_name/$snapshot_name2" ]]

	rpc_cmd bdev_malloc_delete "$malloc_dev"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_hotremove_lvol_store" test_hotremove_lvol_store
run_test "test_hotremove_lvol_store_multiple_lvols" test_hotremove_lvol_store_multiple_lvols
run_test "test_hotremove_lvol_store_base" test_hotremove_lvol_store_base
run_test "test_hotremove_lvol_store_base_with_lvols" test_hotremove_lvol_store_base_with_lvols
run_test "test_bdev_lvol_delete_lvstore_with_clones" test_bdev_lvol_delete_lvstore_with_clones
run_test "test_unregister_lvol_bdev" test_unregister_lvol_bdev

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
