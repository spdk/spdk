#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# Make sure lvol stores are automatically detected after base bdev detach and subsequent attach
function test_tasting() {
	# Create two aio bdevs
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 "$AIO_BS"
	rpc_cmd bdev_aio_create $testdir/aio_bdev_1 aio_bdev1 "$AIO_BS"
	# Create a valid lvs
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test)
	# Destroy lvol store
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	# Remove the lvol stores and make sure it's not being automatically detected after base
	# bdev re-attach.
	rpc_cmd bdev_aio_delete aio_bdev0
	# Create aio bdev on the same file
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 "$AIO_BS"
	sleep 1
	# Check if destroyed lvol store does not exist on aio bdev
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false

	# Create a valid lvs
	lvs1_cluster_size=$((1 * 1024 * 1024))
	lvs2_cluster_size=$((32 * 1024 * 1024))
	lvs_uuid1=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test1 -c $lvs1_cluster_size)
	lvs_uuid2=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev1 lvs_test2 -c $lvs2_cluster_size)

	# Create 5 lvols on first lvs
	lvol_size_mb=$(round_down $((LVS_DEFAULT_CAPACITY_MB / 10)))
	lvol_size=$((lvol_size_mb * 1024 * 1024))

	for i in $(seq 1 5); do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "lvol_test${i}" "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

		[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test1/lvol_test${i}" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / AIO_BS))" ]
	done

	# Create 5 lvols on second lvs
	lvol2_size_mb=$(round_down $(((AIO_SIZE_MB - 16) / 5)) 32)
	lvol2_size=$((lvol2_size_mb * 1024 * 1024))

	for i in $(seq 1 5); do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid2" "lvol_test${i}" "$lvol2_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

		[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test2/lvol_test${i}" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol2_size / AIO_BS))" ]
	done

	old_lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$old_lvols")" == "10" ]
	old_lvs=$(rpc_cmd bdev_lvol_get_lvstores | jq .)

	# Restart spdk app
	killprocess $spdk_pid
	$SPDK_BIN_DIR/spdk_tgt &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# Create aio bdevs
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 "$AIO_BS"
	rpc_cmd bdev_aio_create $testdir/aio_bdev_1 aio_bdev1 "$AIO_BS"
	sleep 1

	# Check tasting feature
	new_lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
	[ "$(jq length <<< "$new_lvols")" == "10" ]
	new_lvs=$(rpc_cmd bdev_lvol_get_lvstores | jq .)
	if ! diff <(jq '. | sort' <<< "$old_lvs") <(jq '. | sort' <<< "$new_lvs"); then
		echo "ERROR: old and loaded lvol store is not the same"
		return 1
	fi
	if ! diff <(jq '. | sort' <<< "$old_lvols") <(jq '. | sort' <<< "$new_lvols"); then
		echo "ERROR: old and loaded lvols are not the same"
		return 1
	fi

	# Check if creation and deletion lvol bdevs on lvs is possible
	for i in $(seq 6 10); do
		lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "lvol_test${i}" "$lvol_size_mb")
		lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

		[ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
		[ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test1/lvol_test${i}" ]
		[ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
		[ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$((lvol_size / AIO_BS))" ]
	done

	for i in $(seq 1 10); do
		rpc_cmd bdev_lvol_delete "lvs_test1/lvol_test${i}"
	done

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid1"

	# Create an lvstore and 10 lvol on top to see if deletion of such struct works as it should.
	lvs_uuid1=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test1)
	for i in $(seq 1 10); do
		rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "lvol_test${i}" "$lvol_size_mb"
	done

	# Clean up
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid1"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid1" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid2"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid2" && false
	rpc_cmd bdev_aio_delete aio_bdev0
	rpc_cmd bdev_aio_delete aio_bdev1
	check_leftover_devices
}

# Positive test for removing lvol store persistently
function test_delete_lvol_store_persistent_positive() {
	local aio0=$testdir/aio_bdev_0
	local bdev_aio_name=${aio0##*/} bdev_block_size=4096
	local lvstore_name=lvstore_test lvstore_uuid

	rpc_cmd bdev_aio_create "$aio0" "$bdev_aio_name" "$bdev_block_size"

	get_bdev_jq bdev_get_bdevs -b "$bdev_aio_name"
	[[ ${jq_out["name"]} == "$bdev_aio_name" ]]
	[[ ${jq_out["product_name"]} == "AIO disk" ]]
	((jq_out["block_size"] == bdev_block_size))

	lvstore_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$bdev_aio_name" "$lvstore_name")

	get_lvs_jq bdev_lvol_get_lvstores -u "$lvstore_uuid"
	[[ ${jq_out["uuid"]} == "$lvstore_uuid" ]]
	[[ ${jq_out["name"]} == "$lvstore_name" ]]
	[[ ${jq_out["base_bdev"]} == "$bdev_aio_name" ]]

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvstore_uuid"
	rpc_cmd bdev_aio_delete "$bdev_aio_name"
	# Create aio bdev on the same file
	rpc_cmd bdev_aio_create "$aio0" "$bdev_aio_name" "$bdev_block_size"
	# Wait 1 second to allow time for lvolstore tasting
	sleep 1
	# bdev_lvol_get_lvstores should not report any existsing lvol stores in configuration
	# after deleting and adding NVMe bdev, thus check if destroyed lvol store does not exist
	# on aio bdev anymore.
	rpc_cmd bdev_lvol_get_lvstores -u "$lvstore_uuid" && false

	# cleanup
	rpc_cmd bdev_aio_delete "$bdev_aio_name"
	check_leftover_devices
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f $testdir/aio_bdev_0 $testdir/aio_bdev_1; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
truncate -s "${AIO_SIZE_MB}M" $testdir/aio_bdev_0 $testdir/aio_bdev_1

run_test "test_tasting" test_tasting
run_test "test_delete_lvol_store_persistent_positive" test_delete_lvol_store_persistent_positive

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
rm -f $testdir/aio_bdev_0 $testdir/aio_bdev_1
