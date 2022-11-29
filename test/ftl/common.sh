#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

# Common utility functions to be sourced by the libftl test scripts

function clear_lvols() {
	stores=$("$rootdir/scripts/rpc.py" bdev_lvol_get_lvstores | jq -r ".[] | .uuid")
	for lvs in $stores; do
		"$rootdir/scripts/rpc.py" bdev_lvol_delete_lvstore -u $lvs
	done
}

function create_nv_cache_bdev() {
	local name=$1
	local cache_bdf=$2
	local base_bdev=$3

	# use 5% space of base bdev
	local size=$(($(get_bdev_size $base_bdev) * 5 / 100))

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local nvc_bdev
	nvc_bdev=$($rootdir/scripts/rpc.py bdev_nvme_attach_controller -b $name -t PCIe -a $cache_bdf)

	local nvc_size
	nvc_size=$(get_bdev_size $nvc_bdev)
	if [[ $size -gt $nvc_size ]]; then
		size=nvc_size
	fi
	$rootdir/scripts/rpc.py bdev_split_create $nvc_bdev -s $size 1
}

function create_base_bdev() {
	local name=$1
	local base_bdf=$2
	local size=$3

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local base_bdev
	base_bdev=$($rootdir/scripts/rpc.py bdev_nvme_attach_controller -b $name -t PCIe -a $base_bdf)

	local base_size
	base_size=$(get_bdev_size $base_bdev)
	if [[ $size -le $base_size ]]; then
		$rootdir/scripts/rpc.py bdev_split_create $base_bdev -s $size 1
	else
		clear_lvols
		lvs=$($rootdir/scripts/rpc.py bdev_lvol_create_lvstore $base_bdev lvs)
		$rootdir/scripts/rpc.py bdev_lvol_create ${base_bdev}p0 $size -t -u $lvs
	fi
}

# Remove not needed files from shared memory
function remove_shm() {
	echo Remove shared memory files
	rm -f rm -f /dev/shm/ftl*
	rm -f rm -f /dev/hugepages/ftl*
	rm -f rm -f /dev/shm/spdk*
	rm -f rm -f /dev/shm/iscsi
	rm -f rm -f /dev/hugepages/spdk*
}
