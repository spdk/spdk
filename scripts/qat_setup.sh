#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
shopt -s nullglob
set -e

rootdir=$(readlink -f $(dirname $0))/..
allowed_drivers=("igb_uio" "uio_pci_generic")

reload_intel_qat() {
	# We need to make sure the out-of-tree intel_qat driver, provided via vm_setup.sh, is in
	# use. Otherwise, some dependency modules loaded by qat_service may fail causing some
	# disturbance later on during the tests - in particular, it's been seen that the adf_ctl
	# was returning inconsistent data (wrong pci addresses), confusing the service into
	# believing SR-IOV is not enabled.

	# If this file doesn't exist, then either intel_qat is a kernel built-in or is not loaded.
	# Nothing to do in such cases, qat_service will load the module for us.
	[[ -e /sys/module/intel_qat/taint ]] || return 0

	local t v
	t=$(< /sys/module/intel_qat/taint)
	v=$(< /sys/module/intel_qat/version)

	# OE - out-of-tree, unsigned. By the very default, drivers built via vm_setup.sh are not
	# signed.
	[[ -z $t || $t != *"OE"* ]] || return 0

	# Check the version of loaded module against the version of the same module as seen
	# from .dep. perspective. if these are the same the most likely something is broken
	# with the dependencies. We report a failure in such a case since reloading the module
	# won't do any good anyway.

	if [[ $(modinfo -F version intel_qat) == "$v" ]]; then
		cat <<- WARN
			Upstream intel_qat driver detected. Same version of the driver is seen
			in modules dependencies: $v. This may mean QAT build didn't update
			dependencies properly. QAT setup may fail, please, rebuild the QAT
			driver.
		WARN
		return 0
	fi

	# Ok, intel_qat is an upstream module, replace it with the out-of-tree one.
	echo "Reloading intel_qat module"

	local h=(/sys/module/intel_qat/holders/*)
	h=("${h[@]##*/}")

	modprobe -r "${h[@]}" intel_qat
	# qat_service does that too, but be vigilant
	modprobe -a intel_qat "${h[@]}"
}

# Please note that this script is not intended to be comprehensive or production quality.
# It supports configuring a single card (the Intel QAT 8970) for use with the SPDK

bad_driver=true
driver_to_bind=uio_pci_generic

reload_intel_qat

_qat_pci_bdfs=(
	/sys/bus/pci/drivers/c6xx/0000*
	/sys/bus/pci/drivers/dh895xcc/0000*
)

qat_pci_bdfs=("${_qat_pci_bdfs[@]#*drivers/}")

if [ ${#qat_pci_bdfs[@]} -eq 0 ]; then
	echo "No QAT devices found. Exiting"
	exit 1
fi

if [ -n "$1" ]; then
	driver_to_bind=$1
fi

for driver in "${allowed_drivers[@]}"; do
	if [ $driver == $driver_to_bind ]; then
		bad_driver=false
	fi
done

if $bad_driver; then
	echo "Unrecognized driver. Please specify an accepted driver (listed below):"
	echo "${allowed_drivers[@]}"
	exit 1
fi

# try starting the qat service. If this doesn't work, just treat it as a warning for now.
if ! service qat_service start; then
	echo "failed to start the qat service. Something may be wrong with your 01.org driver."
fi

expected_num_vfs=0 set_num_vfs=0
# configure virtual functions for the QAT cards.
for qat_bdf in "${qat_pci_bdfs[@]}"; do
	if [[ ! -e /sys/bus/pci/drivers/$qat_bdf/sriov_numvfs ]]; then
		echo "(${qat_bdf##*/}) sriov_numvfs interface missing, is SR-IOV enabled?"
		continue
	fi
	num_vfs=$(< "/sys/bus/pci/drivers/$qat_bdf/sriov_totalvfs")
	echo "$num_vfs" > /sys/bus/pci/drivers/$qat_bdf/sriov_numvfs
	num_vfs_set=$(< "/sys/bus/pci/drivers/$qat_bdf/sriov_numvfs")
	if ((num_vfs != num_vfs_set)); then
		echo "Number of VFs set to $num_vfs_set, expected $num_vfs"
	else
		echo "${qat_bdf##*/} set to $num_vfs VFs"
	fi
	((expected_num_vfs += num_vfs))
	((set_num_vfs += num_vfs_set))
done

# Build VF list out of qat_pci_bdfs[@] by slapping /virtfn* to the path to know if we got'em all.
# shellcheck disable=SC2206 # <- intentional
qat_vf_bdfs=(${_qat_pci_bdfs[@]/%//virtfn*})

if ((expected_num_vfs != set_num_vfs || ${#qat_vf_bdfs[@]} != expected_num_vfs)); then
	echo "Failed to prepare the VFs. Aborting" >&2
	exit 1
fi

modprobe uio

# Insert the dpdk uio kernel module.
if [ $driver_to_bind == "igb_uio" ]; then
	if ! modprobe igb_uio; then
		echo "Unable to insert the igb_uio kernel module. Aborting."
		exit 1
	fi
elif [ "$driver_to_bind" == "uio_pci_generic" ]; then
	modprobe uio_pci_generic
else
	echo "Unsure how to work with driver $driver_to_bind. Please configure it in qat_setup.sh"
	exit 1
fi

# Unbind old driver if necessary.
for vf in "${qat_vf_bdfs[@]}"; do
	vf=$(readlink -f "$vf")
	if [[ -e $vf/driver ]]; then
		old_driver=$(basename "$(readlink -f "$vf/driver")")
		if [[ $old_driver != "$driver_to_bind" ]]; then
			echo "unbinding driver $old_driver from qat VF at BDF ${vf##*/}"
			echo "${vf##*/}" > "/sys/bus/pci/drivers/$old_driver/unbind"
		fi
	fi
	echo "$driver_to_bind" > "$vf/driver_override"
	echo "${vf##*/}" > /sys/bus/pci/drivers_probe
done

echo "Properly configured the qat device with driver $driver_to_bind."
