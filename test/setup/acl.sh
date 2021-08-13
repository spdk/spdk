#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

get_zoned_devs

declare -a devs=()
declare -A drivers=()

collect_setup_devs() {
	local dev driver

	while read -r _ dev _ _ _ driver _; do
		[[ $dev == *:*:*.* ]] || continue
		[[ $driver == nvme ]] || continue
		[[ ${zoned_devs[*]} == *"$dev"* ]] && continue
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
	PCI_BLOCKED="$PCI_BLOCKED ${devs[0]}" setup output config \
		| grep "Skipping denied controller at ${devs[0]}"
	verify "${devs[0]}"
	setup reset
}

allowed() {
	PCI_ALLOWED="${devs[0]}" setup output config \
		| grep -E "${devs[0]} .*: ${drivers["${devs[0]}"]} -> .*"
	verify "${devs[@]:1}"
	setup reset
}

setup reset
collect_setup_devs

run_test "denied" denied
run_test "allowed" allowed
