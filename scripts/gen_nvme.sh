#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=$(iter_pci_class_code 01 08 02)
if [ "$1" != "--json" ]; then
	echo "[Nvme]"
fi

i=0
for bdf in $bdfs; do
	if [ "$1" = "--json" ]; then
		echo "{"
		echo '"subsystem": "bdev",'
		echo '"config": ['
		echo '{'
		echo '"params": {'
		echo '"trtype": "PCIe",'
		echo "\"name\": \"Nvme$i\","
		echo "\"traddr\": \"$bdf\""
		echo '},'
		echo '"method": "construct_nvme_bdev"'
		echo '}'
		echo ']'
		echo '}'
	else
		echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme$i"
	fi

let i=i+1
done
