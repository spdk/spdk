#!/usr/bin/env bash

set -e

function cleanup_linux() {
	# detach pci devices from uio driver
	grep -q "^uio_pci_generic" /proc/modules && rmmod uio_pci_generic

	# bind NVMe devices to NVMe driver if no kernel device
	if [ -d "/sys/bus/pci/drivers/nvme" ]; then
		device=`find /sys/bus/pci/drivers/nvme -name "0000*" -print`
		if [ -z "$device" ]; then
			rmmod nvme
			modprobe nvme
		fi
	fi
}

function cleanup_freebsd {
	kldunload contigmem.ko || true
	kldunload nic_uio.ko || true
}

if [ `uname` = Linux ]; then
	cleanup_linux
else
	cleanup_freebsd
fi
