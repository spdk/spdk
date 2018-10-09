#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))/..
igb_driverdir=$rootdir/dpdk/build/build/kernel/igb_uio/
allowed_drivers=("igb_uio" "uio_pci_generic")

# This script requires an igb_uio kernel module binary located at $igb_driverdir/igb_uio.ko
# Please also note that this script is not intended to be comprehensive or production quality.
# It supports configuring a single card (the Intel QAT 8970) for use with the SPDK

bad_driver=true
driver_to_bind=uio_pci_generic
num_vfs=16
firmware_download_url=http://git.kernel.org/cgit/linux/kernel/git/firmware/linux-firmware.git/tree
qat_binary=qat_895xcc.bin
qat_mmp_binary=qat_895xcc_mmp.bin

qat_pci_bdfs=( $(lspci -Dd:37c8 | awk '{print $1}') )
if [ ${#qat_pci_bdfs[@]} -eq 0 ]; then
	echo "No QAT devices found. Exiting"
	exit 0
fi

if [ ! -z $1 ]; then
	driver_to_bind=$1
fi

for driver in ${allowed_drivers[@]}; do
	if [ $driver == $driver_to_bind ]; then
		bad_driver=false
	fi
done

if $bad_driver; then
	echo "Unrecognized driver. Please specify an accepted driver (listed below):"
	echo "${allowed_drivers[@]}"
	exit 1
fi

# Fetch firmware if needed.
if [ ! -f /lib/firmware/$qat_binary ]; then
	echo "installing qat firmware"
	if ! wget $firmware_download_url/$qat_binary -O /lib/firmware/$qat_binary; then
		echo "Cannot download the qat binary $qat_binary from <$firmware_download_url/$qat_binary>"
		exit 1
	fi
fi

if [ ! -f /lib/firmware/$qat_mmp_binary ]; then
	echo "installing qat mmp firmware"
	if ! wget $firmware_download_url/$qat_mmp_binary -O /lib/firmware/$qat_mmp_binary; then
		echo "Cannot download the qat mmp binary $qat_mmp_binary from <$firmware_download_url/$qat_mmp_binary>"
		exit 1
	fi
fi

# configure virtual functions for the QAT cards.
for qat_bdf in ${qat_pci_bdfs[@]}; do
	echo "$num_vfs" > /sys/bus/pci/drivers/c6xx/$qat_bdf/sriov_numvfs
	num_vfs=$(cat /sys/bus/pci/drivers/c6xx/$qat_bdf/sriov_numvfs)
	echo "$qat_bdf set to $num_vfs VFs"
done

# Confirm we have all of the virtual functions we asked for.

qat_vf_bdfs=( $(lspci -Dd:37c9 | awk '{print $1}') )
if (( ${#qat_vf_bdfs[@]} != ${#qat_pci_bdfs[@]}*num_vfs )); then
	echo "Failed to prepare the VFs. Aborting"
	exit 1
fi

# Unbind old driver if necessary.
for vf in ${qat_vf_bdfs[@]}; do
	old_driver=$(basename $(readlink -f /sys/bus/pci/devices/${vf}/driver))
	if [ $old_driver != "driver" ]; then
		echo "unbinding driver $old_driver from qat VF at BDF $vf"
		echo -n $vf > /sys/bus/pci/drivers/$old_driver/unbind
	fi
done

modprobe uio

# Insert the dpdk uio kernel module.
if [ $driver_to_bind == "igb_uio" ]; then
	if ! lsmod | grep -q igb_uio; then
		if ! insmod $igb_driverdir/igb_uio.ko; then
			echo "Unable to insert the igb_uio kernel module. Aborting."
			exit 1
		fi
	fi
elif [ "$driver_to_bind" == "uio_pci_generic" ]; then
	modprobe uio_pci_generic
else
	echo "Unsure how to work with driver $driver_to_bind. Please configure it in qat_setup.sh"
	exit 1
fi

echo -n "8086 37c9" > /sys/bus/pci/drivers/$driver_to_bind/new_id
for vf in ${qat_vf_bdfs[@]}; do
	if ! ls -l /sys/bus/pci/devices/$vf/driver | grep -q $driver_to_bind; then
		echo "unable to bind the driver to the device at bdf $vf"
		if [ "$driver_to_bind" == "uio_pci_generic" ]; then
			echo "Your kernel's uio_pci_generic module does not support binding to virtual functions."
			echo "It likely is missing Linux git commit ID acec09e67 which is needed to bind"
			echo "uio_pci_generic to virtual functions which have no legacy interrupt vector."
			echo "Please rebuild spdk with --with-igb-uio-driver and re-run this script specifying the igb_uio driver."
		fi
		exit 1
	fi
done
echo "Properly configured the qat device with driver $driver_to_bind."
