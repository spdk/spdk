#!/usr/bin/env bash
shopt -s nullglob

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
num_vfs=16

qat_pci_bdfs=($(lspci -Dd:37c8 | awk '{print $1}'))
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

reload_intel_qat

# try starting the qat service. If this doesn't work, just treat it as a warning for now.
if ! service qat_service start; then
	echo "failed to start the qat service. Something may be wrong with your 01.org driver."
fi

# configure virtual functions for the QAT cards.
for qat_bdf in "${qat_pci_bdfs[@]}"; do
	if [[ ! -e /sys/bus/pci/drivers/c6xx/$qat_bdf/sriov_numvfs ]]; then
		echo "($qat_bdf) sriov_numvfs interface missing, is SR-IOV enabled?"
		continue
	fi
	echo "$num_vfs" > /sys/bus/pci/drivers/c6xx/$qat_bdf/sriov_numvfs
	num_vfs_set=$(cat /sys/bus/pci/drivers/c6xx/$qat_bdf/sriov_numvfs)
	if ((num_vfs != num_vfs_set)); then
		echo "Number of VFs set to $num_vfs_set, expected $num_vfs"
	else
		echo "$qat_bdf set to $num_vfs VFs"
	fi
done

# Confirm we have all of the virtual functions we asked for.

qat_vf_bdfs=($(lspci -Dd:37c9 | awk '{print $1}'))
if ((${#qat_vf_bdfs[@]} != ${#qat_pci_bdfs[@]} * num_vfs)); then
	echo "Failed to prepare the VFs. Aborting"
	exit 1
fi

# Unbind old driver if necessary.
for vf in "${qat_vf_bdfs[@]}"; do
	old_driver=$(basename $(readlink -f /sys/bus/pci/devices/${vf}/driver))
	if [ $old_driver != "driver" ]; then
		echo "unbinding driver $old_driver from qat VF at BDF $vf"
		echo -n $vf > /sys/bus/pci/drivers/$old_driver/unbind
	fi
done

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

echo -n "8086 37c9" > /sys/bus/pci/drivers/$driver_to_bind/new_id
for vf in "${qat_vf_bdfs[@]}"; do
	if ! ls -l /sys/bus/pci/devices/$vf/driver | grep -q $driver_to_bind; then
		echo "unable to bind the driver to the device at bdf $vf"
		if [ "$driver_to_bind" == "uio_pci_generic" ]; then
			echo "Your kernel's uio_pci_generic module does not support binding to virtual functions."
			echo "It likely is missing Linux git commit ID acec09e67 which is needed to bind"
			echo "uio_pci_generic to virtual functions which have no legacy interrupt vector."
			echo "Please build DPDK kernel module for igb_uio and re-run this script specifying the igb_uio driver."
		fi
		exit 1
	fi
done
echo "Properly configured the qat device with driver $driver_to_bind."
