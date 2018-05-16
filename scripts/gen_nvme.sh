#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=($(iter_pci_class_code 01 08 02))
if [ "$1" = "--json" ]; then
        echo "{"
        echo '"subsystem": "bdev",'
        echo '"config": ['
else
        echo "[Nvme]"
fi

for (( i=0; i < ${#bdfs[@]}; i++))
do
        if [ "$1" = "--json" ]; then
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
        else
                echo "  TransportID \"trtype:PCIe traddr:${bdfs[i]}\" Nvme$i"
        fi
done

if [ "$1" = "--json" ]; then
        echo ']'
        echo '}'
fi
