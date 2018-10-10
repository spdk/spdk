#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))/..
igb_driverdir=$rootdir/dpdk/build/build/kernel/igb_uio/
source "$rootdir/test/common/autotest_common.sh"

set +x

# This script requires an igb_uio kernel module binary located at $DEPENDENCY_DIR/igb_uio.ko
# Please also note that this script is not intended to be comprehensive or production quality.
# It supports configuring a single card (the Intel QAT 8970) for use with the SPDK

num_vfs=16
firmware_download_url=http://git.kernel.org/cgit/linux/kernel/git/firmware/linux-firmware.git/tree
qat_binary=qat_895xcc.bin
qat_mmp_binary=qat_895xcc_mmp.bin

num_qat_bus=`lspci -d:37c8 | wc -l`
expected_num_VFs=$(($num_qat_bus * $num_vfs))
if [ $expected_num_VFs -eq 0 ]; then
	echo "No QAT devices found. Exiting"
	exit 0
fi

#Install firmware if needed.
if [ ! -f /lib/firmware/$qat_binary ]; then
	echo "installing qat firmware"
	if ! wget $firmware_download_url/$qat_binary -O /lib/firmware/$qat_binary; then
		echo "Cannot install the qat binary"
		exit 1
	fi
fi

if [ ! -f /lib/firmware/$qat_mmp_binary ]; then
	echo "installing qat mmp firmware"
	if ! wget $firmware_download_url/$qat_binary -O /lib/firmware/$qat_mmp_binary; then
		echo "Cannot install the qat mmp binary"
		exit 1
	fi
fi

# configure virtual functions for the QAT cards.
for qat_bdf in $(lspci -d:37c8 | awk '{print $1}'); do
	echo "$num_vfs" > /sys/bus/pci/drivers/c6xx/0000:$qat_bdf/sriov_numvfs
	num_vfs=`cat /sys/bus/pci/drivers/c6xx/0000:$qat_bdf/sriov_numvfs`
	echo "$qat_bdf set to $num_vfs VFs"
done

# Confirm we have all of the virtual functions we asked for.
num_qat_bus=`lspci -d:37c8 | wc -l`
expected_num_VFs=$(($num_qat_bus * $num_vfs))
if [ `lspci -d:37c9 | wc -l` -lt $expected_num_VFs ]; then
	echo "Failed to prepare the VFs. Aborting"
	exit 1
fi

# Unbind old driver if necessary.
for bus in $(lspci -d:37c8 | cut -d':' -f1); do
	for device in $(lspci -d:37c9 | grep "$bus:" | cut -d':' -f2 | cut -d'.' -f1 | sort | uniq); do
		for fn in $(lspci -d:37c9 | grep "$bus:$device" | cut -d':' -f2 | cut -d'.' -f2 | awk '{print $1}'); do
			echo "unbinding driver from qat VF at BDF 0000:${bus}:${device}.${fn}"
			echo -n 0000:${bus}:${device}.${fn} > \
			/sys/bus/pci/devices/0000\:${bus}\:${device}.${fn}/driver/unbind || true
		done
	done
done

modprobe uio

# Insert the dpdk uio kernel module.
if ! lsmod | grep -q igb_uio; then
	if ! insmod $igb_driverdir/igb_uio.ko; then
		echo "Unable to insert the igb_uio kernel module. Aborting."
		exit 1
	fi
fi

echo "8086 37c9" > /sys/bus/pci/drivers/igb_uio/new_id

if ! lspci -vvd:37c9 | grep -q igb_uio; then
	echo "igb_uio driver not properly loaded. Aborting."
	exit 1
else
	echo "Properly configured the igb_uio kernel module."
fi

set -x

exit 0
