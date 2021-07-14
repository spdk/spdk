#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..
source "$rootdir/scripts/common.sh"

gen_subsystems=false
gen_mode="local"
gen_function="create_local_json_config"
gen_args=()

function usage() {
	echo "Script for generating JSON configuration file for attaching"
	echo "local userspace NVMe drives."
	echo "Usage: ${0##*/} [OPTIONS]"
	echo
	echo "-h, --help                     Print help and exit"
	echo "    --mode                     Generate 'local' or 'remote' NVMe JSON configuration. Default is 'local'."
	echo "                               Remote needs --trid option to be present."
	echo "    --trid                     Comma separated list target subsystem information containing transport type,"
	echo "                               IP addresses, port numbers and NQN names."
	echo "                               Example: tcp:127.0.0.1:4420:nqn.2016-06.io.spdk:cnode1,tcp:127.0.0.1:4421:nqn.2016-06.io.spdk:cnode2"
	echo "    --json-with-subsystems     Wrap bdev subsystem JSON configuration with higher level 'subsystems' dictionary."
	exit 0
}

function create_local_json_config() {
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

function create_remote_json_config() {
	local trids
	local bdev_json_cfg=()

	IFS="," read -r -a trids <<< $1
	for ((i = 0; i < ${#trids[@]}; i++)); do
		local transport
		local ip_addr
		local svc_port
		local nqn

		IFS=":" read -r transport ip_addr svc_port nqn <<< ${trids[i]}
		bdev_json_cfg+=("$(
			cat <<- JSON
				{
					"method": "bdev_nvme_attach_controller",
					"params": {
						"trtype": "$transport",
						"adrfam": "IPv4",
						"name": "Nvme${i}",
						"subnqn": "$nqn",
						"traddr": "$ip_addr",
						"trsvcid": "$svc_port"
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
				mode=*)
					gen_mode="${OPTARG#*=}"
					gen_function="create_${OPTARG#*=}_json_config"
					;;
				trid=*) remote_trid="${OPTARG#*=}" ;;
				json-with-subsystems) gen_subsystems=true ;;
				*) echo "Invalid argument '$OPTARG'" && usage ;;
			esac
			;;
		h) usage ;;
		*) echo "Invalid argument '$OPTARG'" && usage ;;
	esac
done

if [[ "$gen_mode" == "remote" ]] && [[ -z "$remote_trid" ]]; then
	echo "For $gen_mode --trid argument must be provided."
	exit 1
fi

if [[ "$gen_mode" == "remote" ]]; then
	gen_args+=("$remote_trid")
fi

bdev_json_cfg=$("$gen_function" "${gen_args[@]}")
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
