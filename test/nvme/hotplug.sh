#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

[[ $(uname -s) == Linux ]] || exit 0

cleanup() {
	[[ -e /proc/$hotplug_pid/status ]] || return 0
	kill "$hotplug_pid"
}

remove_attach_helper() {
	local hotplug_events=$1
	local hotplug_wait=$2
	local dev

	# We need to make sure we wait long enough for hotplug to initialize the devices
	# and start IO - if we start removing devices before that happens we will end up
	# stepping on hotplug's toes forcing it to fail to report proper count of given
	# events.
	sleep "$hotplug_wait"

	while ((hotplug_events--)); do
		for dev in "${nvmes[@]}"; do
			echo 1 > "/sys/bus/pci/devices/$dev/remove"
		done

		echo 1 > "/sys/bus/pci/rescan"

		# setup.sh masks failures while writing to sysfs interfaces so let's do
		# this on our own to make sure there's anything for the hotplug to reattach
		# to.
		for dev in "${nvmes[@]}"; do
			echo "$dev" > "/sys/bus/pci/devices/$dev/driver/unbind"
			echo "$dev" > "/sys/bus/pci/drivers/${pci_bus_driver["$dev"]}/bind"
		done
		# Wait now for hotplug to reattach to the devices
		sleep "$hotplug_wait"
	done
}

hotplug() {
	local hotplug_events=3
	local hotplug_wait=6 # This should be enough for more stubborn nvmes in the CI

	remove_attach_helper "$hotplug_events" "$hotplug_wait" &
	hotplug_pid=$!

	"$SPDK_EXAMPLE_DIR/hotplug" \
		-i 0 \
		-t $((hotplug_events * hotplug_wait + hotplug_wait)) \
		-n $((hotplug_events * nvme_count)) \
		-r $((hotplug_events * nvme_count)) \
		-l warning
}

"$rootdir/scripts/setup.sh"
nvmes=($(nvme_in_userspace)) nvme_count=${#nvmes[@]}

xtrace_disable
cache_pci_bus_sysfs
xtrace_restore

trap "cleanup" EXIT

hotplug
