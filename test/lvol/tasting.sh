#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh

# Test for tasting feature.
# Create lvol store on aio bdev and delete it. Delete aio bdev
# and add it again. Check if spdk target has no lvol stores.
# Add another aio bdev. Create lvs and lvol bdevs on two aio bdevs.
# Restart spdk, add again aio bdevs and check
# if logical volume configuration was restored and remained unchanged.
# Check if creation and deletion lvol bdevs on lvs is possible.
function test_tasting() {
	# Create two aio bdevs
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 4096
	rpc_cmd bdev_aio_create $testdir/aio_bdev_1 aio_bdev1 4096
	# Create a valid lvs
        lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test)
	# Destroy lvol store
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	# Delete aio bdev
	rpc_cmd bdev_aio_delete aio_bdev0
	# Create aio bdev on the same file
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 4096
	sleep 1
	# Check if destroyed lvol store does not exist on aio bdev
	! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"

	# Create a valid lvs
	lvs1_cluster_size=$(( 1 * 1024 * 1024 ))
	lvs2_cluster_size=$(( 32 * 1024 * 1024 ))
        lvs_uuid1=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test1 -c $lvs1_cluster_size)
	lvs_uuid2=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev1 lvs_test2 -c $lvs2_cluster_size)

	# Create 5 lvols on first lvs
        lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 10 )) )
        lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	for i in $(seq 1 5); do
                lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "lvol_test${i}" "$lvol_size_mb")
                lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

                [ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test1/lvol_test${i}" ]
                [ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
                [ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / AIO_BS ))" ]
        done

	# Create 5 lvols on second lvs
        lvol2_size_mb=$( round_down $(( ( AIO_SIZE_MB - 16 ) / 5 )) 32 )
        lvol2_size=$(( lvol2_size_mb * 1024 * 1024 ))

	for i in $(seq 1 5); do
                lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid2" "lvol_test${i}" "$lvol2_size_mb")
                lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

                [ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test2/lvol_test${i}" ]
                [ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
                [ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol2_size / AIO_BS ))" ]
        done

	old_lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
        [ "$(jq length <<< "$old_lvols")" == "10" ]
	old_lvs=$(rpc_cmd bdev_lvol_get_lvstores)

	# Restart spdk app
	killprocess $spdk_pid
	$rootdir/app/spdk_tgt/spdk_tgt &
	spdk_pid=$!
	waitforlisten $spdk_pid

	# Create aio bdevs
	rpc_cmd bdev_aio_create $testdir/aio_bdev_0 aio_bdev0 4096
	rpc_cmd bdev_aio_create $testdir/aio_bdev_1 aio_bdev1 4096
	sleep 1

	# Check tasting feature
	new_lvols=$(rpc_cmd bdev_get_bdevs | jq -r '[ .[] | select(.product_name == "Logical Volume") ]')
        [ "$(jq length <<< "$new_lvols")" == "10" ]
	new_lvs=$(rpc_cmd bdev_lvol_get_lvstores)
	if diff $old_lvs $new_lvs; then
		echo "ERROR: old and loaded lvol store is not the same"
		return 1
	fi
	if diff $old_lvols $new_lvols; then
                echo "ERROR: old and loaded lvols are not the same"
                return 1
	fi

	# Add another 5 lvol bdevs
	for i in $(seq 6 10); do
                lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid1" "lvol_test${i}" "$lvol_size_mb")
                lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

                [ "$(jq -r '.[0].name' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].uuid' <<< "$lvol")" = "$lvol_uuid" ]
                [ "$(jq -r '.[0].aliases[0]' <<< "$lvol")" = "lvs_test1/lvol_test${i}" ]
                [ "$(jq -r '.[0].block_size' <<< "$lvol")" = "$AIO_BS" ]
                [ "$(jq -r '.[0].num_blocks' <<< "$lvol")" = "$(( lvol_size / AIO_BS ))" ]
        done

	# Delete all lvol bdevs
	for i in $(seq 1 10); do
		rpc_cmd bdev_lvol_delete "lvs_test1/lvol_test${i}"
	done

	# Destroy lvol store
        rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid1"

	# Create a valid lvs
        lvs_uuid1=$(rpc_cmd bdev_lvol_create_lvstore aio_bdev0 lvs_test1)
	# and add ten valid lvol bdevs
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

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f $testdir/aio_bdev_0 $testdir/aio_bdev_1; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
truncate -s 400M $testdir/aio_bdev_0 $testdir/aio_bdev_1

run_test "test_tasting" test_tasting

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
rm -f $testdir/aio_bdev_0 $testdir/aio_bdev_1
