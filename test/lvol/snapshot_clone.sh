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

	# Create two lvol bdevs
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 6 )) )
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
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid2" /dev/nbd0
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE ))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdevs
	snapshot_uuid1=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test1 lvol_snapshot1)
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test2 lvol_snapshot2)

	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid1" /dev/nbd0
	# Try to perform write operation on created snapshot
	# Check if filling snapshot of lvol bdev fails
	count=$(( lvol_size / LVS_DEFAULT_CLUSTER_SIZE ))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs="$LVS_DEFAULT_CLUSTER_SIZE" count=$count && false
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
	cmp /dev/nbd0 /dev/nbd2 && false

	# clean up
	for i in $(seq 0 3); do
		nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd$i
	done

	rpc_cmd bdev_lvol_delete "$lvol_uuid1"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid1" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid1"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid1" && false
	rpc_cmd bdev_lvol_delete "$lvol_uuid2"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid2" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}


function test_create_snapshot_with_io() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 2 )) )
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
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}


function test_create_snapshot_of_snapshot() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 3 )) )

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create snapshot of previously created snapshot
	# and check if operation will fail
	rpc_cmd bdev_lvol_snapshot lvs_test/lvol_snapshot lvol_snapshot2 && false

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid" && false
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_get_bdevs -b "$snapshot_uuid" && false
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check if only clone of snapshot can be created.
# Check if writing to one clone doesn't affect other clone
# Check if relations between clones and snapshots are properly set in configuration
function test_clone_snapshot_relations() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 6 )) )
	lvol_size=$(( lvol_size_mb * 1024 * 1024 ))

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Fill lvol bdev with 100% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create clone of lvol bdev and check if it fails
	rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test && false

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create again clone of lvol bdev and check if it fails
	rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test && false

	# Create two clones of snapshot and check if it ends with success
	clone_uuid1=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_snapshot clone_test1)
	clone_uuid2=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_snapshot clone_test2)

	# Perform write operation to first clone
	# Change first half of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid1" /dev/nbd0
	fill_size=$(( lvol_size / 2 ))
	run_fio_test /dev/nbd0 0 $fill_size "write" "0xaa"

	# Compare snapshot with second clone. Data on both bdevs should be the same
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid" /dev/nbd1
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid2" /dev/nbd2
	sleep 1
	cmp /dev/nbd1 /dev/nbd2
	# Compare snapshot with first clone
	cmp /dev/nbd0 /dev/nbd1 && false

	snapshot_bdev=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_snapshot")
	clone_bdev1=$(rpc_cmd bdev_get_bdevs -b "lvs_test/clone_test1")
	clone_bdev2=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_test")

	# Check snapshot consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot_bdev")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot_bdev")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot_bdev")" = "$(jq '.|sort' <<< '["lvol_test", "clone_test1", "clone_test2"]')" ]

	# Check first clone consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev1")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev1")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev1")" = '"lvol_snapshot"' ]

	# Check second clone consistency
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$clone_bdev2")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$clone_bdev2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.base_snapshot' <<< "$clone_bdev2")" = '"lvol_snapshot"' ]

	# Destroy first clone and check if it is deleted from snapshot
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$clone_uuid1"
	snapshot_bdev=$(rpc_cmd bdev_get_bdevs -b "lvs_test/lvol_snapshot")
	[ "$(jq '.[].driver_specific.lvol.clones|sort' <<< "$snapshot_bdev")" = "$(jq '.|sort' <<< '["lvol_test", "clone_test2"]')" ]

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd2
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete "$clone_uuid2"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_clone_inflate() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 4 )) )

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Fill lvol bdev with 100% of its space
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Create clone of snapshot
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]

	# Fill part of clone with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	first_fill=0
	second_fill=$(( lvol_size * 3 / 4 ))
	run_fio_test /dev/nbd0 first_fill $(( 1024*1024 )) "write" "0xdd"
	run_fio_test /dev/nbd0 second_fill $(( 1024*1024 )) "write" "0xdd"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Do inflate
	rpc_cmd bdev_lvol_inflate lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "false" ]

	# Delete snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 first_fill $(( 1024*1024 )) "read" "0xdd"
	run_fio_test /dev/nbd0 $(( (first_fill + 1) * 1024 * 1024 )) $(( (second_fill - first_fill - 1) * 1024 * 1024 )) "read" "0xcc"
	run_fio_test /dev/nbd0 second_fill $(( 1024*1024 )) "read" "0xdd"
	run_fio_test /dev/nbd0 $(( (second_fill + 1) * 1024 * 1024 )) $(( (lvol_size - second_fill - 1) * 1024 * 1024 )) "read" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Create tree level snaphot-snapshot2-clone structure.
# Detach snapshot2 from clone. Check if snapshot2 can be safely removed.
# Each time check consistency of snapshot-clone relations and written data.
function test_clone_decouple_parent() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$(( 5 * LVS_DEFAULT_CLUSTER_SIZE_MB ))
	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb" -t)
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Decouple parent lvol bdev and check if it fails
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test && false

	# Fill first four out of 5 clusters of clone with data of known pattern
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	begin_fill=0
	end_fill=$(( lvol_size_mb * 4 * 1024 * 1024 / 5 ))
	run_fio_test /dev/nbd0 $begin_fill $end_fill "write" "0xdd"

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill second and fourth cluster of clone with data of known pattern
	start_fill=$(( lvol_size_mb * 1024 * 1024 / 5 ))
	fill_range=$start_fill
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"
	start_fill=$(( lvol_size_mb * 3 * 1024 * 1024 / 5 ))
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xcc"

	# Create second snapshot of thin provisioned lvol bdev
	snapshot_uuid2=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot2)

	# Fill second cluster of clone with data of known pattern
	start_fill=$fill_range
	run_fio_test /dev/nbd0 $start_fill $fill_range "write" "0xee"

	# Check data consistency
	pattern=( "0xdd" "0xee" "0xdd" "0xcc" "0x00" )
	for i in $(seq 0 4); do
		start_fill=$(( lvol_size_mb * i * 1024 * 1024 / 5 ))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" ${pattern[$i]}
	done

	# Decouple parent lvol bdev
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	snapshot2=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot2")" = "true" ]

	# Decouple parent lvol bdev
	rpc_cmd bdev_lvol_decouple_parent lvs_test/lvol_test
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	snapshot=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid")
	snapshot2=$(rpc_cmd bdev_get_bdevs -b "$snapshot_uuid2")
	[ "$(jq '.[].driver_specific.lvol.thin_provision' <<< "$lvol")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$lvol")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot")" = "false" ]
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$snapshot2")" = "true" ]
	[ "$(jq '.[].driver_specific.lvol.snapshot' <<< "$snapshot2")" = "true" ]

	# Check data consistency
	for i in $(seq 0 4); do
		start_fill=$(( lvol_size_mb * i * 1024 * 1024 / 5 ))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" ${pattern[$i]}
	done

	# Delete second snapshot
	rpc_cmd bdev_lvol_delete "$snapshot_uuid2"

	# Check data consistency
	for i in $(seq 0 4); do
		start_fill=$(( lvol_size_mb * i * 1024 * 1024 / 5 ))
		run_fio_test /dev/nbd0 $start_fill $fill_range "read" ${pattern[$i]}
	done

	# Clean up
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Set lvol bdev as read only and perform clone on it.
function test_lvol_bdev_readonly() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 2 )) )

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Set lvol bdev as read only
	rpc_cmd bdev_lvol_set_read_only lvs_test/lvol_test

	# Try to perform write operation on lvol marked as read only
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc" && false
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create clone of lvol set to read only
	clone_uuid=$(rpc_cmd bdev_lvol_clone lvs_test/lvol_test clone_test)

	# Try to perform write operation on lvol clone
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$clone_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc"
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Clean up
	rpc_cmd bdev_lvol_delete "$clone_uuid"
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

# Check if it is possible to delete snapshot with clone
function test_delete_snapshot_with_clone() {
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore "$malloc_name" lvs_test)

	# Calculate size and create lvol bdev
	lvol_size_mb=$( round_down $(( LVS_DEFAULT_CAPACITY_MB / 2 )) )

	lvol_uuid=$(rpc_cmd bdev_lvol_create -u "$lvs_uuid" lvol_test "$lvol_size_mb")
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")

	# Perform write operation on lvol
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid" /dev/nbd0
	run_fio_test /dev/nbd0 0 lvol_size "write" "0xcc"

	# Create snapshots of lvol bdev
	snapshot_uuid=$(rpc_cmd bdev_lvol_snapshot lvs_test/lvol_test lvol_snapshot)

	# Fill first half of lvol bdev
	half_size=$(( lvol_size / 2 - 1 ))
	run_fio_test /dev/nbd0 0 half_size "write" "0xee"

	# Check if snapshot was unchanged
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$snapshot_uuid" /dev/nbd1
	run_fio_test /dev/nbd0 0 half_size "read" "0xcc"

	# Verify lvol bdev
	run_fio_test /dev/nbd0 0 half_size "read" "0xee"
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "true" ]

	# Delete snapshot - should succeed
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd1
	rpc_cmd bdev_lvol_delete "$snapshot_uuid"

	# Check data consistency
	lvol=$(rpc_cmd bdev_get_bdevs -b "$lvol_uuid")
	[ "$(jq '.[].driver_specific.lvol.clone' <<< "$lvol")" = "false" ]
	run_fio_test /dev/nbd0 0 half_size "read" "0xee"
	run_fio_test /dev/nbd0 $(( half_size + 1 )) $lvol_size "read" "0xcc"

	# Clean up
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid"
	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_snapshot_compare_with_lvol_bdev" test_snapshot_compare_with_lvol_bdev
run_test "test_create_snapshot_with_io" test_create_snapshot_with_io
run_test "test_create_snapshot_of_snapshot" test_create_snapshot_of_snapshot
run_test "test_clone_snapshot_relations" test_clone_snapshot_relations
run_test "test_clone_inflate" test_clone_inflate
run_test "test_clone_decouple_parent" test_clone_decouple_parent
run_test "test_lvol_bdev_readonly" test_lvol_bdev_readonly
run_test "test_delete_snapshot_with_clone" test_delete_snapshot_with_clone

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
