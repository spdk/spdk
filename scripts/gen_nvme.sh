#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=$(iter_pci_class_code 01 08 02)

echo "[Nvme]"
i=0
for bdf in $bdfs; do
        echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme$i"
        let i=i+1
done
