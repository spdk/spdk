#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

# Pci bus hotplug
# Helper function to remove/attach cotrollers
debug_remove_attach_helper() {
	local helper_time=0

	helper_time=$(timing_cmd remove_attach_helper "$@")
	printf 'remove_attach_helper took %ss to complete (handling %u nvme drive(s))' \
		"$helper_time" "$nvme_count" >&2
}

remove_attach_helper() {
	local hotplug_events=$1
	local hotplug_wait=$2
	local use_bdev=${3:-false}
	local dev bdfs

	# We need to make sure we wait long enough for hotplug to initialize the devices
	# and start IO - if we start removing devices before that happens we will end up
	# stepping on hotplug's toes forcing it to fail to report proper count of given
	# events.
	sleep "$hotplug_wait"

	while ((hotplug_events--)); do
		for dev in "${nvmes[@]}"; do
			echo 1 > "/sys/bus/pci/devices/$dev/remove"
		done

		if "$use_bdev"; then
			# Since we removed all the devices, when the sleep settles, we expect to find no bdevs
			sleep "$hotplug_wait" && (($(rpc_cmd bdev_get_bdevs | jq 'length') == 0))
		fi || return 1

		# Avoid setup.sh as it does some extra work which is not relevant for this test.
		echo 1 > "/sys/bus/pci/rescan"

		for dev in "${nvmes[@]}"; do
			echo "${pci_bus_driver["$dev"]}" > "/sys/bus/pci/devices/$dev/driver_override"
			echo "$dev" > "/sys/bus/pci/devices/$dev/driver/unbind"
			echo "$dev" > "/sys/bus/pci/drivers_probe"
			echo "" > "/sys/bus/pci/devices/$dev/driver_override"
		done

		# Wait now for hotplug to reattach to the devices
		sleep "$((hotplug_wait * nvme_count))"

		if "$use_bdev"; then
			# See if we get all the bdevs back in one bulk
			bdfs=($(rpc_cmd bdev_get_bdevs | jq -r '.[].driver_specific.nvme[].pci_address' | sort))
			[[ ${bdfs[*]} == "${nvmes[*]}" ]]
		fi
	done
}

run_hotplug() {
	trap 'killprocess $hotplug_pid; exit 1' SIGINT SIGTERM EXIT

	"$SPDK_EXAMPLE_DIR/hotplug" \
		-i 0 \
		-t 0 \
		-n $((hotplug_events * nvme_count)) \
		-r $((hotplug_events * nvme_count)) \
		-l warning &
	hotplug_pid=$!

	debug_remove_attach_helper "$hotplug_events" "$hotplug_wait" false

	# Wait in case hotplug app is lagging behind
	# and kill it, if it hung.
	sleep $hotplug_wait

	if ! kill -0 "$hotplug_pid"; then
		# hotplug already finished, check for the error code.
		wait "$hotplug_pid"
	else
		echo "Killing hotplug application"
		killprocess $hotplug_pid
		return 1
	fi

	trap - SIGINT SIGTERM EXIT
}

# SPDK target hotplug
tgt_run_hotplug() {
	local dev

	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!

	trap 'killprocess ${spdk_tgt_pid}; echo 1 > /sys/bus/pci/rescan; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_tgt_pid

	for dev in "${!nvmes[@]}"; do
		rpc_cmd bdev_nvme_attach_controller -b "Nvme0$dev" -t PCIe -a "${nvmes[dev]}"
		waitforbdev "Nvme0${dev}n1" "$hotplug_wait"
	done

	rpc_cmd bdev_nvme_set_hotplug -e

	debug_remove_attach_helper "$hotplug_events" "$hotplug_wait" true
	# Verify reregistering hotplug poller
	rpc_cmd bdev_nvme_set_hotplug -d
	rpc_cmd bdev_nvme_set_hotplug -e

	debug_remove_attach_helper "$hotplug_events" "$hotplug_wait" true

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_tgt_pid
}

# Preparation
"$rootdir/scripts/setup.sh"

hotplug_wait=6
hotplug_events=3
nvmes=($(nvme_in_userspace))
nvme_count=$((${#nvmes[@]} > 2 ? 2 : ${#nvmes[@]}))
nvmes=("${nvmes[@]::nvme_count}")

xtrace_disable
cache_pci_bus
xtrace_restore

# Run pci bus hotplug test
run_hotplug

# Run SPDK target based hotplug
tgt_run_hotplug
