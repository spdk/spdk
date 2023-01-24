#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname $0)")
rootdir=$(readlink -f "$testdir/../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/lvol/common.sh"

MALLOC_SIZE_MB=128
MALLOC_BS=4096
NUM_QUEUE=4
QUEUE_DEPTH=512
FILE_SIZE=$((MALLOC_SIZE_MB * 1024 * 1024))

function test_create_ublk() {
	# create a ublk target
	ublk_target=$(rpc_cmd ublk_create_target)
	# create a malloc bdev
	malloc_name=$(rpc_cmd bdev_malloc_create $MALLOC_SIZE_MB $MALLOC_BS)
	# Add ublk device
	ublk_id=$(rpc_cmd ublk_start_disk $malloc_name 0 -q $NUM_QUEUE -d $QUEUE_DEPTH)
	ublk_path="/dev/ublkb$ublk_id"
	ublk_dev=$(rpc_cmd ublk_get_disks -n $ublk_id)
	# verify its parameters
	[[ "$(jq -r '.[0].ublk_device' <<< "$ublk_dev")" = "$ublk_path" ]]
	[[ "$(jq -r '.[0].id' <<< "$ublk_dev")" = "$ublk_id" ]]
	[[ "$(jq -r '.[0].queue_depth' <<< "$ublk_dev")" = "$QUEUE_DEPTH" ]]
	[[ "$(jq -r '.[0].num_queues' <<< "$ublk_dev")" = "$NUM_QUEUE" ]]
	[[ "$(jq -r '.[0].bdev_name' <<< "$ublk_dev")" = "$malloc_name" ]]

	# Run fio in background that writes to ublk
	run_fio_test /dev/ublkb0 0 $FILE_SIZE "write" "0xcc" "--time_based --runtime=10"

	# clean up
	rpc_cmd ublk_stop_disk "$ublk_id"
	# make sure we can't delete the same ublk
	NOT rpc_cmd ublk_stop_disk "$ublk_id"
	rpc_cmd ublk_destroy_target

	rpc_cmd bdev_malloc_delete "$malloc_name"
	check_leftover_devices
}

function test_create_multi_ublk() {
	# create a ublk target
	ublk_target=$(rpc_cmd ublk_create_target)

	for i in {0..3}; do
		# create a malloc bdev
		malloc_name=$(rpc_cmd bdev_malloc_create -b "Malloc${i}" $MALLOC_SIZE_MB $MALLOC_BS)
		# add ublk device
		ublk_id=$(rpc_cmd ublk_start_disk $malloc_name "${i}" -q $NUM_QUEUE -d $QUEUE_DEPTH)
	done

	ublk_dev=$(rpc_cmd ublk_get_disks)
	for i in {0..3}; do
		# verify its parameters
		[[ "$(jq -r ".[${i}].ublk_device" <<< "$ublk_dev")" = "/dev/ublkb${i}" ]]
		[[ "$(jq -r ".[${i}].id" <<< "$ublk_dev")" = "${i}" ]]
		[[ "$(jq -r ".[${i}].queue_depth" <<< "$ublk_dev")" = "$QUEUE_DEPTH" ]]
		[[ "$(jq -r ".[${i}].num_queues" <<< "$ublk_dev")" = "$NUM_QUEUE" ]]
		[[ "$(jq -r ".[${i}].bdev_name" <<< "$ublk_dev")" = "Malloc${i}" ]]
	done

	# clean up
	for i in {0..3}; do
		rpc_cmd ublk_stop_disk "${i}"
	done
	rpc_cmd ublk_destroy_target

	for i in {0..3}; do
		rpc_cmd bdev_malloc_delete "Malloc${i}"
	done
	check_leftover_devices
}

function cleanup() {
	killprocess $spdk_pid
}

modprobe ublk_drv
"$SPDK_BIN_DIR/spdk_tgt" -m 0x3 -L ublk &
spdk_pid=$!
trap 'cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

run_test "test_create_ublk" test_create_ublk
run_test "test_create_multi_ublk" test_create_multi_ublk

trap - SIGINT SIGTERM EXIT
cleanup
