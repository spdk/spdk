#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_server=/var/tmp/spdk-raid.sock
tmp_file=$SPDK_TEST_STORAGE/raidrandtest

source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

rpc_py="$rootdir/scripts/rpc.py -s $rpc_server"

function raid_unmap_data_verify() {
	if hash blkdiscard; then
		local nbd=$1
		local rpc_server=$2
		local blksize
		blksize=$(lsblk -o LOG-SEC $nbd | grep -v LOG-SEC | cut -d ' ' -f 5)
		local rw_blk_num=4096
		local rw_len=$((blksize * rw_blk_num))
		local unmap_blk_offs=(0 1028 321)
		local unmap_blk_nums=(128 2035 456)
		local unmap_off
		local unmap_len

		# data write
		dd if=/dev/urandom of=$tmp_file bs=$blksize count=$rw_blk_num
		dd if=$tmp_file of=$nbd bs=$blksize count=$rw_blk_num oflag=direct
		blockdev --flushbufs $nbd

		# confirm random data is written correctly in raid0 device
		cmp -b -n $rw_len $tmp_file $nbd

		for ((i = 0; i < ${#unmap_blk_offs[@]}; i++)); do
			unmap_off=$((blksize * ${unmap_blk_offs[$i]}))
			unmap_len=$((blksize * ${unmap_blk_nums[$i]}))

			# data unmap on tmp_file
			dd if=/dev/zero of=$tmp_file bs=$blksize seek=${unmap_blk_offs[$i]} count=${unmap_blk_nums[$i]} conv=notrunc

			# data unmap on raid bdev
			blkdiscard -o $unmap_off -l $unmap_len $nbd
			blockdev --flushbufs $nbd

			# data verify after unmap
			cmp -b -n $rw_len $tmp_file $nbd
		done
	fi

	return 0
}

function on_error_exit() {
	if [ -n "$raid_pid" ]; then
		killprocess $raid_pid
	fi

	rm -f $tmp_file
	print_backtrace
	exit 1
}

function configure_raid_bdev() {
	local raid_level=$1
	rm -rf $testdir/rpcs.txt

	cat <<- EOL >> $testdir/rpcs.txt
		bdev_malloc_create 32 512 -b Base_1
		bdev_malloc_create 32 512 -b Base_2
		bdev_raid_create -z 64 -r $raid_level -b "Base_1 Base_2" -n raid
	EOL
	$rpc_py < $testdir/rpcs.txt

	rm -rf $testdir/rpcs.txt
}

function raid_function_test() {
	local raid_level=$1
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local nbd=/dev/nbd0
		local raid_bdev

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -L bdev_raid &
		raid_pid=$!
		echo "Process raid pid: $raid_pid"
		waitforlisten $raid_pid $rpc_server

		configure_raid_bdev $raid_level
		raid_bdev=$($rpc_py bdev_raid_get_bdevs online | jq -r '.[0]["name"] | select(.)')
		if [ $raid_bdev = "" ]; then
			echo "No raid0 device in SPDK app"
			return 1
		fi

		nbd_start_disks $rpc_server $raid_bdev $nbd
		count=$(nbd_get_count $rpc_server)
		if [ $count -ne 1 ]; then
			return 1
		fi

		raid_unmap_data_verify $nbd $rpc_server

		nbd_stop_disks $rpc_server $nbd
		count=$(nbd_get_count $rpc_server)
		if [ $count -ne 0 ]; then
			return 1
		fi

		killprocess $raid_pid
	else
		echo "skipping bdev raid tests."
	fi

	return 0
}

function verify_raid_bdev_state() {
	local raid_bdev_name=$1
	local expected_state=$2
	local raid_level=$3
	local strip_size=$4
	local raid_bdev
	local raid_bdev_info
	local num_base_bdevs
	local num_base_bdevs_discovered
	local tmp

	raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[0] | select(.)')
	if [ -z "$raid_bdev" ]; then
		echo "No raid device in SPDK app"
		return 1
	fi

	raid_bdev_info=$($rpc_py bdev_raid_get_bdevs $expected_state | jq -r ".[] | select(.name == \"$raid_bdev_name\")")
	if [ -z "$raid_bdev_info" ]; then
		echo "$raid_bdev_name is not in $expected_state state"
		return 1
	fi

	tmp=$(echo $raid_bdev_info | jq -r '.state')
	if [ "$tmp" != $expected_state ]; then
		echo "incorrect state: $tmp, expected: $expected_state"
		return 1
	fi

	tmp=$(echo $raid_bdev_info | jq -r '.raid_level')
	if [ "$tmp" != $raid_level ]; then
		echo "incorrect level: $tmp, expected: $raid_level"
		return 1
	fi

	tmp=$(echo $raid_bdev_info | jq -r '.strip_size_kb')
	if [ "$tmp" != $strip_size ]; then
		echo "incorrect strip size: $tmp, expected: $strip_size"
		return 1
	fi

	num_base_bdevs=$(echo $raid_bdev_info | jq -r '.base_bdevs_list | length')
	tmp=$(echo $raid_bdev_info | jq -r '.num_base_bdevs')
	if [ "$num_base_bdevs" != "$tmp" ]; then
		echo "incorrect num_base_bdevs: $tmp, expected: $num_base_bdevs"
		return 1
	fi

	num_base_bdevs_discovered=$(echo $raid_bdev_info | jq -r '[.base_bdevs_list[] | strings] | length')
	tmp=$(echo $raid_bdev_info | jq -r '.num_base_bdevs_discovered')
	if [ "$num_base_bdevs_discovered" != "$tmp" ]; then
		echo "incorrect num_base_bdevs_discovered: $tmp, expected: $num_base_bdevs_discovered"
		return 1
	fi
}

function raid_state_function_test() {
	local raid_level=$1
	local raid_bdev
	local base_bdev1="Non_Existed_Base_1"
	local base_bdev2="Non_Existed_Base_2"
	local raid_bdev_name="Existed_Raid"
	local strip_size=64

	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid $rpc_server

	# Step1: create a RAID bdev with no base bdevs
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create -z $strip_size -r $raid_level -b "$base_bdev1 $base_bdev2" -n $raid_bdev_name
	if ! verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size; then
		return 1
	else
		# Test: Delete the RAID bdev successfully
		$rpc_py bdev_raid_delete $raid_bdev_name
	fi

	# Step2: create one base bdev and add to the RAID bdev
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create -z $strip_size -r $raid_level -b "$base_bdev1 $base_bdev2" -n $raid_bdev_name
	$rpc_py bdev_malloc_create 32 512 -b $base_bdev1
	waitforbdev $base_bdev1
	if ! verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size; then
		$rpc_py bdev_malloc_delete $base_bdev1
		$rpc_py bdev_raid_delete $raid_bdev_name
		return 1
	else
		# Test: Delete the RAID bdev successfully
		$rpc_py bdev_raid_delete $raid_bdev_name
	fi

	# Step3: create another base bdev and add to the RAID bdev
	# Expect state: ONLINE
	$rpc_py bdev_raid_create -z $strip_size -r $raid_level -b "$base_bdev1 $base_bdev2" -n $raid_bdev_name
	$rpc_py bdev_malloc_create 32 512 -b $base_bdev2
	waitforbdev $base_bdev2
	if ! verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size; then
		$rpc_py bdev_malloc_delete $base_bdev1
		$rpc_py bdev_malloc_delete $base_bdev2
		$rpc_py bdev_raid_delete $raid_bdev_name
		return 1
	fi

	# Step4: delete one base bdev from the RAID bdev
	# Expect state: OFFLINE
	$rpc_py bdev_malloc_delete $base_bdev2
	if ! verify_raid_bdev_state $raid_bdev_name "offline" $raid_level $strip_size; then
		$rpc_py bdev_malloc_delete $base_bdev1
		$rpc_py bdev_raid_delete $raid_bdev_name
		return 1
	fi

	# Step5: delete last base bdev from the RAID bdev
	# Expect state: removed from system
	$rpc_py bdev_malloc_delete $base_bdev1
	raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[0]["name"] | select(.)')
	if [ -n "$raid_bdev" ]; then
		echo "$raid_bdev_name is not removed"
		$rpc_py bdev_raid_delete $raid_bdev_name
		return 1
	fi

	killprocess $raid_pid

	return 0
}

function raid0_resize_test() {
	local blksize=512
	local bdev_size_mb=32
	local new_bdev_size_mb=$((bdev_size_mb * 2))
	local blkcnt
	local raid_size_mb
	local new_raid_size_mb

	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid $rpc_server

	$rpc_py bdev_null_create Base_1 $bdev_size_mb $blksize
	$rpc_py bdev_null_create Base_2 $bdev_size_mb $blksize

	$rpc_py bdev_raid_create -z 64 -r 0 -b "Base_1 Base_2" -n Raid

	# Resize Base_1 first.
	$rpc_py bdev_null_resize Base_1 $new_bdev_size_mb

	# The size of Raid should not be changed.
	blkcnt=$($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks')
	raid_size_mb=$((blkcnt * blksize / 1048576))
	if [ $raid_size_mb != $((bdev_size_mb * 2)) ]; then
		echo "resize failed"
		return 1
	fi

	# Resize Base_2 next.
	$rpc_py bdev_null_resize Base_2 $new_bdev_size_mb

	# The size of Raid should be updated to the expected value.
	blkcnt=$($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks')
	raid_size_mb=$((blkcnt * blksize / 1048576))
	if [ $raid_size_mb != $((new_bdev_size_mb * 2)) ]; then
		echo "resize failed"
		return 1
	fi

	killprocess $raid_pid

	return 0
}

trap 'on_error_exit;' ERR

raid_function_test raid0
raid_function_test concat
raid_state_function_test raid0
raid_state_function_test concat
raid0_resize_test

rm -f $tmp_file
