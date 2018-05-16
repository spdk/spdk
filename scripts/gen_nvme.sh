#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

bdfs=($(iter_pci_class_code 01 08 02))
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

if [ "$1" = "--json" ]; then
	create_json_config
else
	create_classic_config
fi
