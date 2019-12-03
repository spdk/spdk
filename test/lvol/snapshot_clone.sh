#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

# Check if snapshot and lvol bdev are the same when creating snapshot.
# Check if snapshot differs from lvol if writing to lvol.
# Check if snapshot is readonly.
function test_snapshot_compare_with_lvol_bdev() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create two lvol bdevs on top
        lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 6 ))
        # Round down lvol size to the nearest cluster size boundary
        lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
        lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

        lvol_uuid1=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test1 "$lvol_size_mb" -t)
        lvol1=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid1")
	lvol_uuid2=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test2 "$lvol_size_mb")
        lvol2=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid2")

	# Fill thin provisoned lvol bdev with 50% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" /dev/nbd0
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE / 2 ))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	# Fill whole thick provisioned lvol bdev
	echo "ghsghsghs"
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid2" /dev/nbd0
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE ))
        dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
        nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	echo "weqweqwe"
	# Create snapshots of lvol bdevs
	snapshot_uuid1=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test1 lvol_snapshot1)
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test2 lvol_snapshot2)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid1" /dev/nbd0
	# Try to perform write operation on created snapshot
        # Check if filling snapshot of lvol bdev fails
        count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE ))
        ! dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" /dev/nbd0
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid2" /dev/nbd1
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid1" /dev/nbd2
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid2" /dev/nbd3
	# Compare every lvol bdev with corresponding snapshot
        # and check that data are the same
	cmp /dev/nbd0 /dev/nbd2
	cmp /dev/nbd1 /dev/nbd3

	# Fill second half of thin provisioned lvol bdev
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE / 2 ))
        dd if=/dev/urandom of=/dev/nbd0 oflag=direct seek=$count bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count

	# Compare thin provisioned lvol bdev with its snapshot and check if it fails
	! cmp /dev/nbd0 /dev/nbd2

	# clean up
	for i in $(seq 0 3); do
		nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd$i
	done

        rpc_cmd bdev_lvol_delete "$lvol_uuid1"
        ! rpc_cmd bdev_get_bdevs -b "$lvol_uuid1"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid1"
        ! rpc_cmd bdev_get_bdevs -b "$snapshot_uuid1"
	rpc_cmd bdev_lvol_delete "$lvol_uuid2"
        ! rpc_cmd bdev_get_bdevs -b "$lvol_uuid2"
        rpc_cmd bdev_lvol_delete "$snapshot_uuid2"
        ! rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2"
        rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
        ! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"
        rpc_cmd bdev_malloc_delete "$malloc_name"
}


function test_create_snapshot_with_io() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol bdev
	lvol_size_mb=$(( LVS_DEFAULT_CAPACITY_MB / 2 ))
	# Round down lvol size to the nearest cluster size boundary
	lvol_size_mb=$(( lvol_size_mb / LVS_DEFAULT_CLUSTER_SIZE_MB * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)
	lvol1=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Run fio in background that writes to lvol bdev
        nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc" "--time_based --runtime=8" &
	fio_proc=$!
	sleep 4
	# Create snapshots of lvol bdev
        snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)
	wait $fio_proc

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
        ! rpc_cmd bdev_get_bdevs -b "$lvol_uuid"
        rpc_cmd bdev_lvol_delete "$snapshot_uuid"
        ! rpc_cmd bdev_get_bdevs -b "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
        ! rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid"
        rpc_cmd bdev_malloc_delete "$malloc_name"
}


$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_lvol_test test_snapshot_compare_with_lvol_bdev
run_lvol_test test_create_snapshot_with_io

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
