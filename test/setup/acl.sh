#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

declare -a devs=()
declare -A drivers=()

collect_setup_devs() {
	local dev driver

	while read -r _ dev _ _ _ driver _; do
		[[ $dev == *:*:*.* ]] || continue
		devs+=("$dev") drivers["$dev"]=$driver
	done < <(setup output status)
	((${#devs[@]} > 0))
}

verify() {
	local dev driver

	for dev; do
		[[ -e /sys/bus/pci/devices/$dev ]]
		driver=$(readlink -f "/sys/bus/pci/devices/$dev/driver")
		[[ ${drivers["$dev"]} == "${driver##*/}" ]]
	done
}

denied() {
	# Include OCSSD devices in the PCI_BLOCKED to make sure we don't unbind
	# them from the pci-stub (see autotest.sh for details).
	PCI_BLOCKED="$OCSSD_PCI_DEVICES ${devs[0]}" setup output config \
		| grep "Skipping denied controller at ${devs[0]}"
	verify "${devs[0]}"
	setup reset
}

allowed() {
	PCI_ALLOWED="${devs[0]}" setup output config \
		| grep "Skipping denied controller at " \
		| grep -v "${devs[0]}"
	verify "${devs[@]:1}"
	setup reset
}

setup reset
collect_setup_devs

run_test "denied" denied
run_test "allowed" allowed
