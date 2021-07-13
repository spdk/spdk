#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

gen_subsystems=false

function usage() {
	echo "Script for generating JSON configuration file for attaching"
	echo "local userspace NVMe drives."
	echo "Usage: ${0##*/} [OPTIONS]"
	echo
	echo "-h, --help                     Print help and exit"
	echo "    --json-with-subsystems     Wrap bdev subsystem JSON configuration with higher level 'subsystems' dictionary."
	exit 0
}

function create_json_config() {
	local bdev_json_cfg=()
	local bdfs=()

	bdfs=($(nvme_in_userspace))

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

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage ;;
				json-with-subsystems) gen_subsystems=true ;;
				*) echo "Invalid argument '$OPTARG'" && usage ;;
			esac
			;;
		h) usage ;;
		*) echo "Invalid argument '$OPTARG'" && usage ;;
	esac
done

bdev_json_cfg=$(create_json_config)
if [[ $gen_subsystems == true ]]; then
	bdev_json_cfg=$(
		cat <<- JSON
			{
				"subsystems": [
					$bdev_json_cfg
				]
			}
		JSON
	)
fi

echo "$bdev_json_cfg"
