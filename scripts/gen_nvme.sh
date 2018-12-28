#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=($(iter_pci_class_code 01 08 02))
function check_nvme_driver()
{
	# Check used drivers. If it's not vfio-pci or uio-pci-generic
	# then most likely PCI_WHITELIST option was used for setup.sh
	# and we do not want to use that disk.
	local new_bdfs=""
	for bdf in ${bdfs[@]}; do
		driver=`grep DRIVER /sys/bus/pci/devices/$bdf/uevent | awk -F"=" '{print $2}'`
		if [ "$driver" != "nvme" ]; then
			new_bdfs+="$bdf "
		fi
	done
	bdfs=($new_bdfs)
}

function create_classic_config()
{
	echo "[Nvme]"
	for (( i=0; i < ${#bdfs[@]}; i++))
	do
		echo "  TransportID \"trtype:PCIe traddr:${bdfs[i]}\" Nvme$i"
	done
}

function create_json_config()
{
	echo "{"
	echo '"subsystem": "bdev",'
	echo '"config": ['
	for (( i=0; i < ${#bdfs[@]}; i++))
	do
		echo '{'
		echo '"params": {'
		echo '"trtype": "PCIe",'
		echo "\"name\": \"Nvme$i\","
		echo "\"traddr\": \"${bdfs[i]}\""
		echo '},'
		echo '"method": "construct_nvme_bdev"'
		if [ -z ${bdfs[i+1]} ]; then
			echo '}'
		else
			echo '},'
		fi
	done
	echo ']'
	echo '}'
}

check_nvme_driver
if [ "$1" = "--json" ]; then
	create_json_config
else
	create_classic_config
fi
