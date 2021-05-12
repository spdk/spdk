#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)

source $rootdir/test/ocf/common.sh

function fio_verify() {
	fio_bdev $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev "$@"
}

function cleanup() {
	rm -f $curdir/modes.conf
}

# Clear nvme device which we will use in test
clear_nvme

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

# Building config is not backtrace worthy ...
xtrace_disable

config=() ocf_names=() ocf_modes=()

ocf_names[1]=PT_Nvme ocf_modes[1]=pt
ocf_names[2]=WT_Nvme ocf_modes[2]=wt
ocf_names[3]=WB_Nvme0 ocf_modes[3]=wb
ocf_names[4]=WB_Nvme1 ocf_modes[4]=wb

mapfile -t config < <("$rootdir/scripts/gen_nvme.sh")

# Drop anything from last closing ] so we can inject our own config pieces ...
config=("${config[@]::${#config[@]}-2}")
# ... and now convert entire array to a single string item
config=("${config[*]}")

config+=(
	"$(
		cat <<- JSON
			{
			  "method": "bdev_split_create",
			  "params": {
			    "base_bdev": "Nvme0n1",
			     "split_count": 8,
			     "split_size_mb": 101
			    }
			}
		JSON
	)"
)

for ((d = 0, c = 1; d <= ${#ocf_names[@]} + 2; d += 2, c++)); do
	config+=(
		"$(
			cat <<- JSON
				{
				  "method": "bdev_ocf_create",
				    "params": {
				      "name": "${ocf_names[c]}",
				      "mode": "${ocf_modes[c]}",
				      "cache_bdev_name": "Nvme0n1p$d",
				      "core_bdev_name": "Nvme0n1p$((d + 1))"
				    }
				}
			JSON
		)"
	)
done

config+=(
	"$(
		cat <<- JSON
			{
			  "method": "bdev_wait_for_examine"
			}
		JSON
	)"
)

# First ']}' closes our config and bdev subsystem blocks
cat <<- CONFIG > "$curdir/modes.conf"
	{"subsystems":[
	$(
	IFS=","
	printf '%s\n' "${config[*]}"
	)
	]}]}
CONFIG

# Format the config nicely and dump it to stdout for everyone to marvel at it ...
jq . "$curdir/modes.conf"

# ... and now back to our regularly scheduled program
xtrace_restore

fio_verify --filename=PT_Nvme:WT_Nvme:WB_Nvme0:WB_Nvme1 --spdk_json_conf="$curdir/modes.conf" --thread=1

trap - SIGINT SIGTERM EXIT
cleanup
