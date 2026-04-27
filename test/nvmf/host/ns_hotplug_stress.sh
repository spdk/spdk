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

function attach_controller() {
	for ((i = 0; i < io_paths_nr; i++)); do
		$rpc_py bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -f IPv4 -s "$((NVMF_PORT + i))" -n "$NVME_SUBNQN" -b nvme0
	done
}

function get_bdev_size_s() {
	xtrace_disable
	get_bdev_size "${@}"
	xtrace_restore
}

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
		$tgt_rpc <<- EOF
			nvmf_subsystem_add_ns -n $current $NVME_SUBNQN ${bdev_name}
			nvmf_subsystem_remove_ns $NVME_SUBNQN $current
		EOF

		# Check if intiator is still alive, otherwise we'd wait until all threads finish
		kill -s 0 "$spdk_app_pid"
	done

	# No resizes are expected here. Should catch cases where nsdata
	# was zeroed out for inactive namespace, that incorrectly was assigned
	# to bdev_nvme.
	[[ "$(get_resize_count)" == 0 ]]

	$tgt_rpc bdev_null_delete "${bdev_name}"
}

add_remove_resize() {
	local nsid=$1 thread=$2
	local inc_nsid dec_nsid
	local max_namespaces=$ns_per_thread
	local bdev_name="null${thread}"

	local resize_bdev_size="$bdev_size"
	local stable_bdev_size="$((bdev_size - thread))"

	# Split into two non-overlapping halves so that the "inc" and "dec"
	# add/remove paths never reuse the same NSID. Each NSID is used
	# exactly once -- no NSID that was removed is ever re-added. This
	# avoids a race where the initiator is still processing the AER for
	# a removed NSID when the target re-adds a namespace at that same
	# NSID.
	local inc_nsid_base=$nsid
	local dec_nsid_base=$((nsid + 2 * max_namespaces - 1))
	local resize_nsid="$((nsid + --max_namespaces))"
	local stable_nsid="$((nsid + --max_namespaces))"

	# Two bdevs will be added/removed with increasing and decreasing NSIDs
	# respectively, forcing the log page to contain entries with diverging
	# NSID ordering.
	# A third bdev will be increased in size during the test, forcing AERs
	# for namespace attribute changes other than add/remove.
	# A fourth bdev will remain at fixed size, to verify that it persist
	# throughout the test and no changes to its attributes were made. Size
	# is unique among all threads.
	$tgt_rpc <<- EOF
		bdev_null_create ${bdev_name}_inc $bdev_size $blk_size
		bdev_null_create ${bdev_name}_dec $bdev_size $blk_size
		bdev_null_create ${bdev_name}_resize $resize_bdev_size $blk_size
		nvmf_subsystem_add_ns -n $resize_nsid $NVME_SUBNQN ${bdev_name}_resize
		bdev_null_create ${bdev_name}_stable $stable_bdev_size $blk_size
		nvmf_subsystem_add_ns -n $stable_nsid $NVME_SUBNQN ${bdev_name}_stable
	EOF

	for ((i = 0; i < max_namespaces; i++)); do
		inc_nsid=$((inc_nsid_base + i))
		dec_nsid=$((dec_nsid_base - i))
		((++resize_bdev_size))

		$tgt_rpc <<- EOF
			nvmf_subsystem_add_ns -n $inc_nsid $NVME_SUBNQN ${bdev_name}_inc
			nvmf_subsystem_remove_ns $NVME_SUBNQN $inc_nsid
			nvmf_subsystem_add_ns -n $dec_nsid $NVME_SUBNQN ${bdev_name}_dec
			nvmf_subsystem_remove_ns $NVME_SUBNQN $dec_nsid
			bdev_null_resize ${bdev_name}_resize $resize_bdev_size
		EOF

		# Wait for updated size on the initiator
		waitforcondition '[[ "$(get_bdev_size_s nvme0n${resize_nsid})" == "$resize_bdev_size" ]]'

		# Check if namespace for stable bdev still has the same size
		[[ "$(get_bdev_size_s nvme0n$stable_nsid)" == "$stable_bdev_size" ]]

		# Check if intiator is still alive, otherwise we'd wait until all threads finish
		kill -s 0 "$spdk_app_pid"
	done

	$tgt_rpc <<- EOF
		nvmf_subsystem_remove_ns $NVME_SUBNQN $stable_nsid
		nvmf_subsystem_remove_ns $NVME_SUBNQN $resize_nsid
		bdev_null_delete ${bdev_name}_inc
		bdev_null_delete ${bdev_name}_dec
		bdev_null_delete ${bdev_name}_resize
		bdev_null_delete ${bdev_name}_stable
	EOF
}

test_case() {
	local fn=$1 stride_nsid=$2 admin_poll_period_us=$3
	local start_nsid i pids=()

	$rpc_py bdev_nvme_set_options --nvme-adminq-poll-period-us "$admin_poll_period_us"
	attach_controller

	for ((i = 0; i < nthreads; ++i)); do
		# Every thread can use stride_nsid NSIDs starting at specific offset.
		start_nsid="$((1 + (stride_nsid * i)))"
		"$fn" "$start_nsid" "$i" &
		pids+=($!)
	done
	wait "${pids[@]}"

	$rpc_py bdev_nvme_detach_controller nvme0
}

nvmftestinit
DEFAULT_RPC_ADDR="$tgt_sock" nvmfappstart -r "$tgt_sock" -m 0x1

io_paths_nr=10

$tgt_rpc nvmf_create_transport $NVMF_TRANSPORT_OPTS
$tgt_rpc nvmf_create_subsystem "$NVME_SUBNQN" -a -s SPDK00000000000001 -m 512
for ((i = 0; i < io_paths_nr; i++)); do
	$tgt_rpc nvmf_subsystem_add_listener "$NVME_SUBNQN" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$((NVMF_PORT + i))"
done

run_app_bg "${SPDK_APP[@]}" -m 0x2
spdk_app_pid=$!
trap 'killprocess $spdk_app_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten "$spdk_app_pid"

# Run several subsystem_{add,remove}_ns RPCs in parallel to ensure they'll get queued
nthreads=6
ns_per_thread=15
bdev_size=100
blk_size=4096

# Test case 1 - add/remove ns with the default admin queue poll period.
run_test "nvmf_ns_hotplug_stress_tc1" test_case add_remove "$ns_per_thread" 10000

# Test case 2 - add/remove ns with delay so AERs pile up on the initiator side.
run_test "nvmf_ns_hotplug_stress_tc2" test_case add_remove "$ns_per_thread" 1000000

# Test case 3 - add/remove/resize ns.
# Each thread needs 2 * ns_per_thread NSIDs: the lower half drives the increasing-NSID
# add/remove path (and the resize, stable namespaces), the upper half drives the "dec" path.
run_test "nvmf_ns_hotplug_stress_tc3" test_case add_remove_resize "$((2 * ns_per_thread))" 10000

waitforlisten "$spdk_app_pid"
killprocess "$spdk_app_pid"
trap - SIGINT SIGTERM EXIT

nvmftestfini
