#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=$(iter_pci_class_code 01 08 02)
if [ -z "$1" ]; then
	echo "[Nvme]"
fi

i=0
for bdf in $bdfs; do
	if [ -n "$1" ]; then
		name=Nvme$i
		cat $1 | jq --arg bdf $bdf --arg name $name -r '(.subsystems[] | select(.subsystem=="bdev").config) += [{"params": {"trtype": "PCIe", "name": $name, "traddr": $bdf}, "method": "construct_nvme_bdev"}]'
	else
        echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme$i"
    fi
        let i=i+1
done
