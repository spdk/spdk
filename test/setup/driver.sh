#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

shopt -s nullglob extglob

dep() { modprobe --show-depends "$1"; }
mod() { [[ $(dep "$1") == *".ko"* ]]; }
bui() { [[ $(dep "$1") == "builtin $1" ]]; }
is_driver() { mod "$1" || bui "$1"; }

uio() {
	is_driver uio_pci_generic
}

vfio() {
	local iommu_grups
	local unsafe_vfio

	[[ -e /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]] \
		&& unsafe_vfio=$(< /sys/module/vfio/parameters/enable_unsafe_noiommu_mode)

	iommu_groups=(/sys/kernel/iommu_groups/*)

	if ((${#iommu_groups[@]} > 0)) || [[ $unsafe_vfio == Y ]]; then
		is_driver vfio_pci && return 0
	fi
	return 1
}

igb_uio() {
	is_driver igb_uio
}

pick_driver() {
	if vfio; then
		echo "vfio-pci"
	elif uio; then
		# Consider special case for broken uio_pci_generic driver
		if igb_uio; then
			echo "@(uio_pci_generic|igb_uio)"
		else
			echo "uio_pci_generic"
		fi
	elif igb_uio; then
		echo "igb_uio"
	else
		echo "No valid driver found"
	fi
}

guess_driver() {
	local driver setup_driver marker
	local fail=0

	driver=$(pick_driver)

	if [[ $driver == "No valid driver found" ]]; then
		[[ $(setup output config) == "$driver"* ]]
		return 0
	fi

	echo "Looking for driver=$driver"
	while read -r _ _ _ _ marker setup_driver; do
		[[ $marker == "->" ]] || continue
		# Allow for more open matching
		# shellcheck disable=SC2053
		[[ $setup_driver == $driver ]] || fail=1
	done < <(setup output config)

	((fail == 0))
	setup reset
}

setup reset
run_test "guess_driver" "guess_driver"
