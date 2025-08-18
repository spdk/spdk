#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir="$(readlink -f "$(dirname "$0")")"
rootdir=$(readlink -f "$testdir"/../../..)
source "$rootdir/test/common/autotest_common.sh"
rpc_py="$rootdir/scripts/rpc.py"

"$rootdir"/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!

trap 'killprocess $bdev_svc_pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $bdev_svc_pid

# LVS, Bdev and aio disk all are cleaned up at the end of grow call.

# Ensures unique diskpath, bdev and lvs name at start.
disk_unique=$(uuidgen)
aio_disk="/tmp/$disk_unique.img"
bdev_name=$(uuidgen)
lvs_name=$(uuidgen)
block_size=512

function create_lvs() {
	size_bytes=$1
	truncate -s "$size_bytes" "$aio_disk"
	$rpc_py bdev_aio_create "$aio_disk" "$bdev_name" $block_size
	$rpc_py bdev_lvol_create_lvstore "$bdev_name" "$lvs_name" -c "$2" --md_pages_per_cluster_ratio="$3"
}

function grow_lvs() {
	# Get the max growable size of LVS.
	growable_upto=$($rpc_py bdev_lvol_get_lvstores -l "$lvs_name" | jq -r '.[0].max_growable_size')
	num_blocks=$($rpc_py bdev_get_bdevs -b "$bdev_name" | jq -r '.[0].num_blocks')
	current_size=$((num_blocks * block_size))
	cluster_size=$($rpc_py bdev_lvol_get_lvstores -l "$lvs_name" | jq -r '.[0].cluster_size')
	capacity_before=$(($($rpc_py bdev_lvol_get_lvstores -l "$lvs_name" \
		| jq -r '.[0].total_data_clusters') * cluster_size))
	# Grow the underlying device to the max growable size.
	truncate -s +$((growable_upto - current_size)) "$aio_disk"
	$rpc_py bdev_aio_rescan "$bdev_name"
	# Grow the LVS.
	$rpc_py bdev_lvol_grow_lvstore -l "$lvs_name"
	# Verify lvs capacity increase after successful grow.
	capacity_after=$(($($rpc_py bdev_lvol_get_lvstores -l "$lvs_name" \
		| jq -r '.[0].total_data_clusters') * cluster_size))
	[[ $capacity_after -gt $capacity_before ]] || {
		echo "Device size has not changed"
		exit 1
	}
	# max_growable_size is the absolute growable limit. Even 1 cluster worth of grow now should fail.
	truncate -s +"$cluster_size" "$aio_disk"
	$rpc_py bdev_aio_rescan "$bdev_name"
	NOT "$rpc_py" bdev_lvol_grow_lvstore -l "$lvs_name"
	# Verify lvs capacity remains same after grow fails.
	capacity_after_fail=$(($($rpc_py bdev_lvol_get_lvstores -l "$lvs_name" \
		| jq -r '.[0].total_data_clusters') * cluster_size))
	[[ $capacity_after_fail -eq $capacity_after ]] || {
		echo "Device size has changed"
		exit 1
	}
	# Cleanup lvs, bdev and disk.
	$rpc_py bdev_lvol_delete_lvstore -l "$lvs_name"
	$rpc_py bdev_aio_delete "$bdev_name"
	rm -f "$aio_disk"
}

# LVS backed by 100GiB device, 4MiB cluster size and 100 mum_md_pages_cluster_ratio.
create_lvs $((100 * 1024 * 1024 * 1024)) $((4 * 1024 * 1024)) 100
grow_lvs

# LVS backed by 200GiB device, 8MiB cluster size and 200 mum_md_pages_cluster_ratio.
create_lvs $((200 * 1024 * 1024 * 1024)) $((8 * 1024 * 1024)) 200
grow_lvs

# LVS backed by 300GiB device, 16MiB cluster size and 300 mum_md_pages_cluster_ratio.
create_lvs $((300 * 1024 * 1024 * 1024)) $((16 * 1024 * 1024)) 300
grow_lvs

# LVS backed by 500GiB device, 12MiB cluster size and 600 mum_md_pages_cluster_ratio.
create_lvs $((500 * 1024 * 1024 * 1024)) $((12 * 1024 * 1024)) 600
grow_lvs

# LVS backed by 1024GiB device, 254MiB cluster size and 800 mum_md_pages_cluster_ratio.
create_lvs $((1024 * 1024 * 1024 * 1024)) $((254 * 1024 * 1024)) 800
grow_lvs

killprocess $bdev_svc_pid
trap - SIGINT SIGTERM EXIT
