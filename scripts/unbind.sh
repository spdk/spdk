#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function prep_nvme {
	TMP=`mktemp`
	# Get vendor_id:device_id by nvme's class_id and subcalss_id
	lspci -n | awk -F " " '{if ($2 == "0108:") {print $3}}' \
	| awk -F ":" '{print $1" "$2}' > $TMP

	cat $TMP | while read device
	do
		echo $device > /sys/bus/pci/drivers/uio_pci_generic/new_id
	done
	rm $TMP
}

function prep_ioat {
	TMP=`mktemp`
	#collect all the device_id info of ioat devices.
	grep "PCI_DEVICE_ID_INTEL_IOAT" $rootdir/lib/ioat/ioat_pci.h \
	| awk -F"x" '{print $2}' > $TMP
	vendor=8086

	for device in `cat $TMP`
	do
		if lspci -n | grep "$vendor:$device"
		then
			echo $vendor $device > /sys/bus/pci/drivers/uio_pci_generic/new_id
		fi
	done
	rm $TMP
}

function configure_linux {
	rmmod nvme || true
	rmmod uio_pci_generic || true
	rmmod ioatdma || true
	modprobe uio_pci_generic || true
	prep_nvme
	prep_ioat
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

if [ `uname` = Linux ]; then
	configure_linux
else
	configure_freebsd
fi

