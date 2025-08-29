#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 Nutanix Inc. All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"
tgt_sock="/var/tmp/tgt.sock"
tgt_rpc="$rpc_py -s $tgt_sock"

function get_resize_count() {
	# Always use id zero to get notifications from the begining of time
	local notify_id=0

	event_count="$($rpc_py notify_get_notifications -i $notify_id | jq '. | map(select(.type == "bdev_resize")) | length')"

	echo $event_count
}

add_remove() {
	local nsid=$1 thread=$2
	local current
	local bdev_name="null${thread}"

	$tgt_rpc bdev_null_create "${bdev_name}" "$bdev_size" "$blk_size"

	for ((i = 0; i < ns_per_thread; i++)); do
		current=$((nsid + i))
		$tgt_rpc nvmf_subsystem_add_ns -n "$current" "$NVME_SUBNQN" "${bdev_name}"
		$tgt_rpc nvmf_subsystem_remove_ns "$NVME_SUBNQN" "$current"

		# Check if intiator is still alive, otherwise we'd wait until all threads finish
		kill -s 0 "$spdk_app_pid"
	done

	# No resizes are expected here. Should catch cases where nsdata
	# was zeroed out for inactive namespace, that incorrectly was assigned
	# to bdev_nvme.
	[[ "$(get_resize_count)" == 0 ]]

	$tgt_rpc bdev_null_delete "${bdev_name}"
}

nvmftestinit
DEFAULT_RPC_ADDR="$tgt_sock" nvmfappstart -r "$tgt_sock" -m 0x1

$tgt_rpc nvmf_create_transport $NVMF_TRANSPORT_OPTS
$tgt_rpc nvmf_create_subsystem "$NVME_SUBNQN" -a -s SPDK00000000000001 -m 512
$tgt_rpc nvmf_subsystem_add_listener "$NVME_SUBNQN" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

"${SPDK_APP[@]}" -m 0x2 "${NO_HUGE[@]}" &
spdk_app_pid=$!
trap 'killprocess $spdk_app_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten "$spdk_app_pid"

# Run several subsystem_{add,remove}_ns RPCs in parallel to ensure they'll get queued
nthreads=8 pids=()
ns_per_thread=20
bdev_size=100
blk_size=4096

# Test case 1 - add/remove ns with delay for processing admin queue

# Instead of default 10ms, use 1 second timeout
$rpc_py bdev_nvme_set_options --nvme-adminq-poll-period-us 1000000

$rpc_py bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -f IPv4 -s "$NVMF_PORT" -n "$NVME_SUBNQN" -b nvme0

for ((i = 0; i < nthreads; ++i)); do
	# Every thread can use ns_per_thread NSIDs starting at specific offset.
	start_nsid="$((1 + (ns_per_thread * i)))"
	add_remove "$start_nsid" "$i" &
	pids+=($!)
done
wait "${pids[@]}"

# Reattach controller with restored admin queue poll period to 10ms
$rpc_py bdev_nvme_detach_controller nvme0
$rpc_py bdev_nvme_set_options --nvme-adminq-poll-period-us 10000
$rpc_py bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -f IPv4 -s "$NVMF_PORT" -n "$NVME_SUBNQN" -b nvme0

# Test case 2 - add/remove ns constantly

for ((i = 0; i < nthreads; ++i)); do
	# Every thread can use ns_per_thread NSIDs starting at specific offset.
	start_nsid="$((1 + (ns_per_thread * i)))"
	add_remove "$start_nsid" "$i" &
	pids+=($!)
done
wait "${pids[@]}"

waitforlisten "$spdk_app_pid"
killprocess "$spdk_app_pid"
trap - SIGINT SIGTERM EXIT

nvmftestfini
