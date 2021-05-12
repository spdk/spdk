#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/ocf/common.sh

source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

$rootdir/scripts/setup.sh

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
			    "split_count": 7,
			    "split_size_mb": 128
			  }
			}
		JSON
	)"
)

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
jq . <<- CONFIG > "$curdir/config"
	{"subsystems":[
	$(
	IFS=","
	printf '%s\n' "${config[*]}"
	)
	]}]}
CONFIG

# Clear nvme device which we will use in test
clear_nvme

"$SPDK_BIN_DIR/iscsi_tgt" --json "$curdir/config" &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py bdev_ocf_create ocfWT wt Nvme0n1p0 Nvme0n1p1
$rpc_py bdev_ocf_create ocfPT pt Nvme0n1p2 Nvme0n1p3
$rpc_py bdev_ocf_create ocfWB0 wb Nvme0n1p4 Nvme0n1p5
$rpc_py bdev_ocf_create ocfWB1 wb Nvme0n1p4 Nvme0n1p6

# Sorting bdevs because we dont guarantee that they are going to be
# in the same order after shutdown
($rpc_py bdev_ocf_get_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
"$SPDK_BIN_DIR/iscsi_tgt" --json "$curdir/config" &
spdk_pid=$!

trap 'killprocess $spdk_pid; rm -f $curdir/config ocf_bdevs ocf_bdevs_verify; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_pid
sleep 5

# OCF should be loaded now as well

($rpc_py bdev_ocf_get_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs_verify

diff ocf_bdevs ocf_bdevs_verify

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm -f $curdir/config ocf_bdevs ocf_bdevs_verify

clear_nvme $bdf
