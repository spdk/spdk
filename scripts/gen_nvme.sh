#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

function create_classic_config() {
	echo "[Nvme]"
	for ((i = 0; i < ${#bdfs[@]}; i++)); do
		echo "  TransportID \"trtype:PCIe traddr:${bdfs[i]}\" Nvme$i"
	done
}

function create_json_config() {
	local bdev_json_cfg=()

	for i in "${!bdfs[@]}"; do
		bdev_json_cfg+=("$(
			cat <<- JSON
				{
					"method": "bdev_nvme_attach_controller",
					"params": {
						"trtype": "PCIe",
						"name":"Nvme${i}",
						"traddr":"${bdfs[i]}"
					}
				}
			JSON
		)")
	done

	local IFS=","
	cat <<- JSON
		{
			"subsystem": "bdev",
			"config": [
				${bdev_json_cfg[*]}
			]
		}
	JSON
}

function create_json_config_with_subsystems() {
	cat <<- JSON
		{
			"subsystems": [
				$(create_json_config)
			]
		}
	JSON
}

bdfs=($(nvme_in_userspace))

if [ "$1" = "--json" ]; then
	create_json_config
elif [ "$1" = "--json-with-subsystems" ]; then
	create_json_config_with_subsystems
else
	create_classic_config
fi
