#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function linux_iter_pci {
	# Argument is the class code
	# TODO: More specifically match against only class codes in the grep
	# step.
	lspci -mm -n | grep $1 | tr -d '"' | awk -F " " '{print "0000:"$1}'
}

function configure_linux {
	driver_name=vfio-pci
	if [ -z "$(ls /sys/kernel/iommu_groups)" ]; then
		# No IOMMU. Use uio.
		driver_name=uio_pci_generic
	fi

	# NVMe
	modprobe $driver_name || true
	for bdf in $(linux_iter_pci 0108); do
		if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
			# Unbind the device from whatever driver it is currently bound to
			echo $bdf > "/sys/bus/pci/devices/$bdf/driver/unbind"
		fi
		# Bind this device to the new driver
		ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')
		echo "Binding $bdf ($ven_dev_id) to $driver_name"
		echo $ven_dev_id > "/sys/bus/pci/drivers/$driver_name/new_id"
	done


	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/lib/ioat/ioat_pci.h \
	| awk -F"x" '{print $2}' > $TMP

	for dev_id in `cat $TMP`; do
		# Abuse linux_iter_pci by giving it a device ID instead of a class code
		for bdf in $(linux_iter_pci $dev_id); do
			if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
				# Unbind the device from whatever driver it is currently bound to
				echo $bdf > "/sys/bus/pci/devices/$bdf/driver/unbind"
			fi
			# Bind this device to the new driver
			ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')
			echo "Binding $bdf ($ven_dev_id) to $driver_name"
			echo $ven_dev_id > "/sys/bus/pci/drivers/$driver_name/new_id"
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"
}

function reset_linux {
	# NVMe
	modprobe nvme || true
	for bdf in $(linux_iter_pci 0108); do
		ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')
		if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
			# Unregister this device from the driver it was bound to.
			echo $ven_dev_id > "/sys/bus/pci/devices/$bdf/driver/remove_id"
			# Unbind the device from whatever driver it is currently bound to
			echo $bdf > "/sys/bus/pci/devices/$bdf/driver/unbind"
		fi
		# Bind this device back to the nvme driver
		echo "Binding $bdf ($ven_dev_id) to nvme"
		echo $bdf > "/sys/bus/pci/drivers/nvme/bind"
	done


	# IOAT
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/lib/ioat/ioat_pci.h \
	| awk -F"x" '{print $2}' > $TMP

	modprobe ioatdma || true
	for dev_id in `cat $TMP`; do
		# Abuse linux_iter_pci by giving it a device ID instead of a class code
		for bdf in $(linux_iter_pci $dev_id); do
			ven_dev_id=$(lspci -n -s $bdf | cut -d' ' -f3 | sed 's/:/ /')
			if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
				# Unregister this device from the driver it was bound to.
				echo $ven_dev_id > "/sys/bus/pci/devices/$bdf/driver/remove_id"
				# Unbind the device from whatever driver it is currently bound to
				echo $bdf > "/sys/bus/pci/devices/$bdf/driver/unbind"
			fi
			# Bind this device back to the nvme driver
			echo "Binding $bdf ($ven_dev_id) to ioatdma"
			echo $bdf > "/sys/bus/pci/drivers/ioatdma/bind"
		done
	done
	rm $TMP

	echo "1" > "/sys/bus/pci/rescan"
}

function configure_freebsd {
	TMP=`mktemp`
	AWK_PROG="{if (count > 0) printf \",\"; printf \"%s:%s:%s\",\$2,\$3,\$4; count++}"
	echo $AWK_PROG > $TMP
	PCICONF=`pciconf -l | grep 'class=0x010802\|^ioat'`
	BDFS=`echo $PCICONF | awk -F: -f $TMP`
	kldunload nic_uio.ko || true
	kenv hw.nic_uio.bdfs=$BDFS
	kldload nic_uio.ko
	rm $TMP
}

function reset_freebsd {
	kldunload contigmem.ko || true
	kldunload nic_uio.ko || true
}

mode=$1
if [ "$mode" == "" ]; then
	mode="config"
fi

if [ `uname` = Linux ]; then
	if [ "$mode" == "config" ]; then
		configure_linux
	elif [ "$mode" == "reset" ]; then
		reset_linux
	fi
else
	if [ "$mode" == "config" ]; then
		configure_freebsd
	elif [ "$mode" == "reset" ]; then
		reset_freebsd
	fi
fi

