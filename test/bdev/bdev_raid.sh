#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
tmp_dir=$SPDK_TEST_STORAGE/raidtest
tmp_file=$tmp_dir/raidrandtest

source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

rpc_py=rpc_cmd

function raid_unmap_data_verify() {
	if hash blkdiscard; then
		local nbd=$1
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

function cleanup() {
	if [ -n "$raid_pid" ] && ps -p $raid_pid > /dev/null; then
		killprocess $raid_pid
	fi

	rm -rf "$tmp_dir"
}

function raid_unmap_function_test() {
	local raid_level=$1
	local nbd=/dev/nbd0
	local raid_bdev

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b Base_1
	$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b Base_2

	if [ "$raid_level" = "raid1" ]; then
		$rpc_py bdev_raid_create -r $raid_level -b "'Base_1 Base_2'" -n raid
	else
		$rpc_py bdev_raid_create -z 64 -r $raid_level -b "'Base_1 Base_2'" -n raid
	fi

	raid_bdev=$($rpc_py bdev_raid_get_bdevs online | jq -r '.[0]["name"] | select(.)')
	if [ $raid_bdev = "" ]; then
		echo "No raid0 device in SPDK app"
		return 1
	fi

	nbd_start_disks $DEFAULT_RPC_ADDR $raid_bdev $nbd
	count=$(nbd_get_count $DEFAULT_RPC_ADDR)
	if [ $count -ne 1 ]; then
		return 1
	fi

	raid_unmap_data_verify $nbd

	nbd_stop_disks $DEFAULT_RPC_ADDR $nbd
	count=$(nbd_get_count $DEFAULT_RPC_ADDR)
	if [ $count -ne 0 ]; then
		return 1
	fi

	killprocess $raid_pid

	return 0
}

function raid_io_resource_dependence() {
	rm -rf $testdir/aio1.bdev
	rm -rf $testdir/aio2.bdev
	truncate -s 2048MB $testdir/aio1.bdev
	truncate -s 2048MB $testdir/aio2.bdev

	local nbd=/dev/nbd0
	local raid_bdev

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	$rpc_py bdev_aio_create $testdir/aio1.bdev Base_1 4096 --fallocate
	$rpc_py bdev_aio_create $testdir/aio2.bdev Base_2 4096 --fallocate

	$rpc_py bdev_raid_create -r raid1 -b "'Base_1 Base_2'" -n raid

	$rpc_py bdev_lvol_create_lvstore raid lvs -c 8192

	for lv_size in 256 768; do
		echo "==== Testing LV size: ${lv_size} ===="

		$rpc_py bdev_lvol_create -l lvs lv0 ${lv_size} -t

		lvol_bdev=$($rpc_py bdev_lvol_get_lvols | jq -r '.[0]["alias"] | select(.)')
		if [ -z "$lvol_bdev" ]; then
			echo "lvol create failed"
			exit 1
		fi

		nbd_start_disks $DEFAULT_RPC_ADDR $lvol_bdev $nbd
		count=$(nbd_get_count $DEFAULT_RPC_ADDR)
		if [ $count -ne 1 ]; then
			exit 1
		fi

		fio --name=test --filename=$nbd --rw=randwrite --bs=8k --iodepth=128 --ioengine=aio --numjobs=1 --direct=1
		sleep 1

		nbd_stop_disks $DEFAULT_RPC_ADDR $nbd
		count=$(nbd_get_count $DEFAULT_RPC_ADDR)
		if [ $count -ne 0 ]; then
			exit 1
		fi

		time $rpc_py bdev_lvol_delete $lvol_bdev

		echo "==== Done LV size: ${lv_size} ===="
		echo
	done

	killprocess $raid_pid

	rm -rf $testdir/aio1.bdev
	rm -rf $testdir/aio2.bdev

	return 0
}

function verify_raid_bdev_state() {
	local raid_bdev_name=$1
	local expected_state=$2
	local raid_level=$3
	local strip_size=$4
	local num_base_bdevs_operational=$5
	local raid_bdev_info
	local num_base_bdevs
	local num_base_bdevs_discovered
	local tmp

	raid_bdev_info=$($rpc_py bdev_raid_get_bdevs all | jq -r ".[] | select(.name == \"$raid_bdev_name\")")

	xtrace_disable
	if [ -z "$raid_bdev_info" ]; then
		echo "No raid device \"$raid_bdev_name\" in SPDK app"
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

	num_base_bdevs=$(echo $raid_bdev_info | jq -r '[.base_bdevs_list[]] | length')
	tmp=$(echo $raid_bdev_info | jq -r '.num_base_bdevs')
	if [ "$num_base_bdevs" != "$tmp" ]; then
		echo "incorrect num_base_bdevs: $tmp, expected: $num_base_bdevs"
		return 1
	fi

	num_base_bdevs_discovered=$(echo $raid_bdev_info | jq -r '[.base_bdevs_list[] | select(.is_configured)] | length')
	tmp=$(echo $raid_bdev_info | jq -r '.num_base_bdevs_discovered')
	if [ "$num_base_bdevs_discovered" != "$tmp" ]; then
		echo "incorrect num_base_bdevs_discovered: $tmp, expected: $num_base_bdevs_discovered"
		return 1
	fi

	tmp=$(echo $raid_bdev_info | jq -r '.num_base_bdevs_operational')
	if [ "$num_base_bdevs_operational" != "$tmp" ]; then
		echo "incorrect num_base_bdevs_operational $tmp, expected: $num_base_bdevs_operational"
		return 1
	fi

	xtrace_restore
}

function verify_raid_bdev_process() {
	local raid_bdev_name=$1
	local process_type=$2
	local target=$3
	local raid_bdev_info

	raid_bdev_info=$($rpc_py bdev_raid_get_bdevs all | jq -r ".[] | select(.name == \"$raid_bdev_name\")")

	[[ $(jq -r '.process.type // "none"' <<< "$raid_bdev_info") == "$process_type" ]]
	[[ $(jq -r '.process.target // "none"' <<< "$raid_bdev_info") == "$target" ]]
}

function verify_raid_bdev_properties() {
	local raid_bdev_name=$1
	local raid_bdev_info
	local base_bdev_names
	local name
	local cmp_raid_bdev cmp_base_bdev

	raid_bdev_info=$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq '.[]')
	base_bdev_names=$(jq -r '.driver_specific.raid.base_bdevs_list[] | select(.is_configured == true).name' <<< "$raid_bdev_info")
	cmp_raid_bdev=$(jq -r '[.block_size, .md_size, .md_interleave, .dif_type] | join(" ")' <<< "$raid_bdev_info")

	for name in $base_bdev_names; do
		cmp_base_bdev=$($rpc_py bdev_get_bdevs -b $name | jq -r '.[] | [.block_size, .md_size, .md_interleave, .dif_type] | join(" ")')
		[[ "$cmp_raid_bdev" == "$cmp_base_bdev" ]]
	done
}

function has_redundancy() {
	case $1 in
		"raid1" | "raid5f") return 0 ;;
		*) return 1 ;;
	esac
}

function raid_state_function_test() {
	local raid_level=$1
	local num_base_bdevs=$2
	local superblock=$3
	local raid_bdev
	local base_bdevs=($(for ((i = 1; i <= num_base_bdevs; i++)); do echo BaseBdev$i; done))
	local raid_bdev_name="Existed_Raid"
	local strip_size
	local strip_size_create_arg
	local superblock_create_arg

	if [ $raid_level != "raid1" ]; then
		strip_size=64
		strip_size_create_arg="-z $strip_size"
	else
		strip_size=0
	fi

	if [ $superblock = true ]; then
		superblock_create_arg="-s"
	else
		superblock_create_arg=""
	fi

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	# Step1: create a RAID bdev with no base bdevs
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name
	verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
	$rpc_py bdev_raid_delete $raid_bdev_name

	# Step2: create one base bdev and add to the RAID bdev
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name
	$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${base_bdevs[0]}
	waitforbdev ${base_bdevs[0]}
	verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
	$rpc_py bdev_raid_delete $raid_bdev_name

	# Step3: create remaining base bdevs and add to the RAID bdev
	# Expect state: ONLINE
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name
	for ((i = 1; i < num_base_bdevs; i++)); do
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${base_bdevs[$i]}
		waitforbdev ${base_bdevs[$i]}
	done
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs
	verify_raid_bdev_properties $raid_bdev_name

	# Step4: delete one base bdev from the RAID bdev
	$rpc_py bdev_malloc_delete ${base_bdevs[0]}
	local expected_state
	if ! has_redundancy $raid_level; then
		expected_state="offline"
	else
		expected_state="online"
	fi
	verify_raid_bdev_state $raid_bdev_name $expected_state $raid_level $strip_size $((num_base_bdevs - 1))

	# Step5: delete remaining base bdevs from the RAID bdev
	# Expect state: removed from system
	for ((i = 1; i < num_base_bdevs; i++)); do
		raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[0]["name"]')
		if [ "$raid_bdev" != $raid_bdev_name ]; then
			echo "$raid_bdev_name removed before all base bdevs were deleted"
			return 1
		fi
		$rpc_py bdev_malloc_delete ${base_bdevs[$i]}
	done
	raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[0]["name"] | select(.)')
	if [ -n "$raid_bdev" ]; then
		echo "$raid_bdev_name is not removed"
		return 1
	fi

	if [ $num_base_bdevs -gt 2 ]; then
		# Test removing and re-adding base bdevs when in CONFIGURING state
		for ((i = 1; i < num_base_bdevs; i++)); do
			$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${base_bdevs[$i]}
			waitforbdev ${base_bdevs[$i]}
		done
		$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs

		$rpc_py bdev_raid_remove_base_bdev ${base_bdevs[1]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[1].is_configured') == "false" ]]

		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${base_bdevs[0]}
		waitforbdev ${base_bdevs[0]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[0].is_configured') == "true" ]]

		$rpc_py bdev_raid_remove_base_bdev ${base_bdevs[2]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[2].is_configured') == "false" ]]

		$rpc_py bdev_raid_add_base_bdev $raid_bdev_name ${base_bdevs[2]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[2].is_configured') == "true" ]]

		$rpc_py bdev_malloc_delete ${base_bdevs[0]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[0].is_configured') == "false" ]]

		$rpc_py bdev_raid_add_base_bdev $raid_bdev_name ${base_bdevs[1]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		[[ $($rpc_py bdev_raid_get_bdevs all | jq '.[0].base_bdevs_list[1].is_configured') == "true" ]]

		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b NewBaseBdev -u "$($rpc_py bdev_raid_get_bdevs all | jq -r '.[0].base_bdevs_list[0].uuid')"
		waitforbdev NewBaseBdev
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs
		verify_raid_bdev_properties $raid_bdev_name

		$rpc_py bdev_raid_delete $raid_bdev_name
	fi

	killprocess $raid_pid

	return 0
}

function raid_resize_test() {
	local raid_level=$1
	local blksize=$base_blocklen
	local bdev_size_mb=32
	local new_bdev_size_mb=$((bdev_size_mb * 2))
	local blkcnt
	local raid_size_mb
	local new_raid_size_mb
	local expected_size

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	$rpc_py bdev_null_create Base_1 $bdev_size_mb $blksize
	$rpc_py bdev_null_create Base_2 $bdev_size_mb $blksize

	if [ $raid_level -eq 0 ]; then
		$rpc_py bdev_raid_create -z 64 -r $raid_level -b "'Base_1 Base_2'" -n Raid
	else
		$rpc_py bdev_raid_create -r $raid_level -b "'Base_1 Base_2'" -n Raid
	fi

	# Resize Base_1 first.
	$rpc_py bdev_null_resize Base_1 $new_bdev_size_mb

	# The size of Raid should not be changed.
	blkcnt=$($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks')
	raid_size_mb=$((blkcnt * blksize / 1048576))
	if [ $raid_level -eq 0 ]; then
		expected_size=$((bdev_size_mb * 2))
	else
		expected_size=$bdev_size_mb
	fi
	if [ $raid_size_mb != $expected_size ]; then
		echo "resize failed"
		return 1
	fi

	# Resize Base_2 next.
	$rpc_py bdev_null_resize Base_2 $new_bdev_size_mb

	# The size of Raid should be updated to the expected value.
	blkcnt=$($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks')
	raid_size_mb=$((blkcnt * blksize / 1048576))
	if [ $raid_level -eq 0 ]; then
		expected_size=$((new_bdev_size_mb * 2))
	else
		expected_size=$new_bdev_size_mb
	fi
	if [ $raid_size_mb != $expected_size ]; then
		echo "resize failed"
		return 1
	fi

	killprocess $raid_pid

	return 0
}

function raid_superblock_test() {
	local raid_level=$1
	local num_base_bdevs=$2
	local base_bdevs_malloc=()
	local base_bdevs_pt=()
	local base_bdevs_pt_uuid=()
	local raid_bdev_name="raid_bdev1"
	local strip_size
	local strip_size_create_arg
	local raid_bdev_uuid
	local raid_bdev

	if [ $raid_level != "raid1" ]; then
		strip_size=64
		strip_size_create_arg="-z $strip_size"
	else
		strip_size=0
	fi

	"$rootdir/test/app/bdev_svc/bdev_svc" -L bdev_raid &
	raid_pid=$!
	waitforlisten $raid_pid

	# Create base bdevs
	for ((i = 1; i <= num_base_bdevs; i++)); do
		local bdev_malloc="malloc$i"
		local bdev_pt="pt$i"
		local bdev_pt_uuid="00000000-0000-0000-0000-00000000000$i"

		base_bdevs_malloc+=($bdev_malloc)
		base_bdevs_pt+=($bdev_pt)
		base_bdevs_pt_uuid+=($bdev_pt_uuid)

		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b $bdev_malloc
		$rpc_py bdev_passthru_create -b $bdev_malloc -p $bdev_pt -u $bdev_pt_uuid
	done

	# Create RAID bdev with superblock
	$rpc_py bdev_raid_create $strip_size_create_arg -r $raid_level -b "'${base_bdevs_pt[*]}'" -n $raid_bdev_name -s
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs
	verify_raid_bdev_properties $raid_bdev_name

	# Get RAID bdev's UUID
	raid_bdev_uuid=$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq -r '.[] | .uuid')
	if [ -z "$raid_bdev_uuid" ]; then
		return 1
	fi

	# Stop the RAID bdev
	$rpc_py bdev_raid_delete $raid_bdev_name
	raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[]')
	if [ -n "$raid_bdev" ]; then
		return 1
	fi

	# Delete the passthru bdevs
	for i in "${base_bdevs_pt[@]}"; do
		$rpc_py bdev_passthru_delete $i
	done
	if [ "$($rpc_py bdev_get_bdevs | jq -r '[.[] | select(.product_name == "passthru")] | any')" == "true" ]; then
		return 1
	fi

	# Try to create new RAID bdev from malloc bdevs
	# Should fail due to superblock still present on base bdevs
	NOT $rpc_py bdev_raid_create $strip_size_create_arg -r $raid_level -b "'${base_bdevs_malloc[*]}'" -n $raid_bdev_name

	raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[]')
	if [ -n "$raid_bdev" ]; then
		return 1
	fi

	# Re-add first base bdev
	$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[0]} -p ${base_bdevs_pt[0]} -u ${base_bdevs_pt_uuid[0]}

	# Check if the RAID bdev was assembled from superblock
	verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs

	if [ $num_base_bdevs -gt 2 ]; then
		# Re-add the second base bdev and remove it again
		$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[1]} -p ${base_bdevs_pt[1]} -u ${base_bdevs_pt_uuid[1]}
		$rpc_py bdev_passthru_delete ${base_bdevs_pt[1]}
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
	fi

	# Re-add remaining base bdevs
	for ((i = 1; i < num_base_bdevs; i++)); do
		$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}
	done

	# Check if the RAID bdev is in online state
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs
	verify_raid_bdev_properties $raid_bdev_name

	# Check if the RAID bdev has the same UUID as when first created
	if [ "$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq -r '.[] | .uuid')" != "$raid_bdev_uuid" ]; then
		return 1
	fi

	if has_redundancy $raid_level; then
		# Delete one base bdev
		$rpc_py bdev_passthru_delete ${base_bdevs_pt[0]}

		# Check if the RAID bdev is in online state (degraded)
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))

		# Stop the RAID bdev
		$rpc_py bdev_raid_delete $raid_bdev_name
		raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[]')
		if [ -n "$raid_bdev" ]; then
			return 1
		fi

		# Delete remaining base bdevs
		for ((i = 1; i < num_base_bdevs; i++)); do
			$rpc_py bdev_passthru_delete ${base_bdevs_pt[$i]}
		done

		# Re-add base bdevs from the second up to (not including) the last one
		for ((i = 1; i < num_base_bdevs - 1; i++)); do
			$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}

			# Check if the RAID bdev is in configuring state
			verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $((num_base_bdevs - 1))
		done

		# Re-add the last base bdev
		i=$((num_base_bdevs - 1))
		$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}

		# Check if the RAID bdev is in online state (degraded)
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))

		# Stop the RAID bdev
		$rpc_py bdev_raid_delete $raid_bdev_name
		raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[]')
		if [ -n "$raid_bdev" ]; then
			return 1
		fi

		if [ $num_base_bdevs -gt 2 ]; then
			# Delete the last base bdev
			i=$((num_base_bdevs - 1))
			$rpc_py bdev_passthru_delete ${base_bdevs_pt[$i]}
		fi

		# Re-add first base bdev
		# This is the "failed" device and contains the "old" version of the superblock
		$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[0]} -p ${base_bdevs_pt[0]} -u ${base_bdevs_pt_uuid[0]}

		if [ $num_base_bdevs -gt 2 ]; then
			# Check if the RAID bdev is in configuring state
			# This should use the newer superblock version and have n-1 online base bdevs
			verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $((num_base_bdevs - 1))
			[[ $($rpc_py bdev_raid_get_bdevs configuring | jq -r '.[].base_bdevs_list[0].is_configured') == "false" ]]

			# Re-add the last base bdev
			$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}
		fi

		# Check if the RAID bdev is in online state (degraded)
		# This should use the newer superblock version and have n-1 online base bdevs
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))
		[[ $($rpc_py bdev_raid_get_bdevs online | jq -r '.[].base_bdevs_list[0].is_configured') == "false" ]]

		# Check if the RAID bdev has the same UUID as when first created
		if [ "$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq -r '.[] | .uuid')" != "$raid_bdev_uuid" ]; then
			return 1
		fi
	fi

	killprocess $raid_pid

	return 0
}

function raid_rebuild_test() {
	local raid_level=$1
	local num_base_bdevs=$2
	local superblock=$3
	local background_io=$4
	local verify=$5
	local base_bdevs=($(for ((i = 1; i <= num_base_bdevs; i++)); do echo BaseBdev$i; done))
	local raid_bdev_name="raid_bdev1"
	local strip_size
	local create_arg
	local raid_bdev_size
	local data_offset

	if [ $raid_level != "raid1" ]; then
		if [ $background_io = true ]; then
			echo "skipping rebuild test with io for level $raid_level"
			return 1
		fi
		strip_size=64
		create_arg+=" -z $strip_size"
	else
		strip_size=0
	fi

	if [ $superblock = true ]; then
		create_arg+=" -s"
	fi

	"$rootdir/build/examples/bdevperf" -T $raid_bdev_name -t 60 -w randrw -M 50 -o 3M -q 2 -U -z -L bdev_raid &
	raid_pid=$!
	waitforlisten $raid_pid

	# Create base bdevs
	for bdev in "${base_bdevs[@]}"; do
		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${bdev}_malloc
		$rpc_py bdev_passthru_create -b ${bdev}_malloc -p $bdev
	done

	# Create spare bdev
	$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b "spare_malloc"
	$rpc_py bdev_delay_create -b "spare_malloc" -d "spare_delay" -r 0 -t 0 -w 100000 -n 100000
	$rpc_py bdev_passthru_create -b "spare_delay" -p "spare"

	# Create RAID bdev
	$rpc_py bdev_raid_create $create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs

	# Get RAID bdev's size
	raid_bdev_size=$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq -r '.[].num_blocks')

	# Get base bdev's data offset
	data_offset=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[0].data_offset')

	if [ $background_io = true ]; then
		# Start user I/O
		"$rootdir/examples/bdev/bdevperf/bdevperf.py" perform_tests &
	elif [ $verify = true ]; then
		local write_unit_size

		# Write random data to the RAID bdev
		nbd_start_disks $DEFAULT_RPC_ADDR $raid_bdev_name /dev/nbd0
		if [ $raid_level = "raid5f" ]; then
			write_unit_size=$((strip_size * 2 * (num_base_bdevs - 1)))
			echo $((base_blocklen * write_unit_size / 1024)) > /sys/block/nbd0/queue/max_sectors_kb
		else
			write_unit_size=1
		fi
		dd if=/dev/urandom of=/dev/nbd0 bs=$((base_blocklen * write_unit_size)) count=$((raid_bdev_size / write_unit_size)) oflag=direct
		nbd_stop_disks $DEFAULT_RPC_ADDR /dev/nbd0
	fi

	# Remove one base bdev
	$rpc_py bdev_raid_remove_base_bdev ${base_bdevs[0]}

	# Check if the RAID bdev is in online state (degraded)
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))

	# Add bdev for rebuild
	$rpc_py bdev_raid_add_base_bdev $raid_bdev_name "spare"
	sleep 1

	# Check if rebuild started
	verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"

	# Remove the rebuild target bdev
	$rpc_py bdev_raid_remove_base_bdev "spare"

	# Check if the RAID bdev is in online state (degraded)
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))

	# Check if rebuild was stopped
	verify_raid_bdev_process $raid_bdev_name "none" "none"

	# Again, start the rebuild
	$rpc_py bdev_raid_add_base_bdev $raid_bdev_name "spare"
	sleep 1
	verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"

	if [ $superblock = true ] && [ $with_io = false ]; then
		# Stop the RAID bdev
		$rpc_py bdev_raid_delete $raid_bdev_name
		[[ $($rpc_py bdev_raid_get_bdevs all | jq 'length') == 0 ]]

		# Remove the passthru base bdevs, then re-add them to assemble the raid bdev again
		for ((i = 0; i < num_base_bdevs; i++)); do
			$rpc_py bdev_passthru_delete ${base_bdevs[$i]}
		done
		for ((i = 0; i < num_base_bdevs; i++)); do
			$rpc_py bdev_passthru_create -b ${base_bdevs[$i]}_malloc -p ${base_bdevs[$i]}
		done

		# Check if the RAID bdev is in online state (degraded)
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))

		# Check if rebuild is not started
		verify_raid_bdev_process $raid_bdev_name "none" "none"

		# Again, start the rebuild
		$rpc_py bdev_raid_add_base_bdev $raid_bdev_name "spare"
		sleep 1
		verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"
	fi

	local num_base_bdevs_operational=$num_base_bdevs

	if [ $raid_level = "raid1" ] && [ $num_base_bdevs -gt 2 ]; then
		# Remove one more base bdev (not rebuild target)
		$rpc_py bdev_raid_remove_base_bdev ${base_bdevs[1]}

		# Ignore this bdev later when comparing data
		base_bdevs[1]=""
		((num_base_bdevs_operational--))

		# Check if rebuild is still running
		verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"
	fi

	# Wait for rebuild to finish
	local timeout=$((SECONDS + 30))
	while ((SECONDS < timeout)); do
		if ! verify_raid_bdev_process $raid_bdev_name "rebuild" "spare" > /dev/null; then
			break
		fi
		sleep 1
	done

	# Check if rebuild is not running and the RAID bdev has the correct number of operational devices
	verify_raid_bdev_process $raid_bdev_name "none" "none"
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs_operational

	# Stop the RAID bdev
	$rpc_py bdev_raid_delete $raid_bdev_name
	[[ $($rpc_py bdev_raid_get_bdevs all | jq 'length') == 0 ]]

	if [ $verify = true ]; then
		if [ $background_io = true ]; then
			# Compare data on the rebuilt and other base bdevs
			nbd_start_disks $DEFAULT_RPC_ADDR "spare" "/dev/nbd0"
			for bdev in "${base_bdevs[@]:1}"; do
				if [ -z "$bdev" ]; then
					continue
				fi
				nbd_start_disks $DEFAULT_RPC_ADDR $bdev "/dev/nbd1"
				cmp -i $((data_offset * base_blocklen)) /dev/nbd0 /dev/nbd1
				nbd_stop_disks $DEFAULT_RPC_ADDR "/dev/nbd1"
			done
			nbd_stop_disks $DEFAULT_RPC_ADDR "/dev/nbd0"
		else
			# Compare data on the removed and rebuilt base bdevs
			nbd_start_disks $DEFAULT_RPC_ADDR "${base_bdevs[0]} spare" "/dev/nbd0 /dev/nbd1"
			cmp -i $((data_offset * base_blocklen)) /dev/nbd0 /dev/nbd1
			nbd_stop_disks $DEFAULT_RPC_ADDR "/dev/nbd0 /dev/nbd1"
		fi
	fi

	if [ $superblock = true ]; then
		# Remove then re-add a base bdev to assemble the raid bdev again
		$rpc_py bdev_passthru_delete "spare"
		$rpc_py bdev_passthru_create -b "spare_delay" -p "spare"
		$rpc_py bdev_wait_for_examine

		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs_operational
		verify_raid_bdev_process $raid_bdev_name "none" "none"
		[[ $($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[0].name') == "spare" ]]

		# Remove and re-add a base bdev - rebuild should start automatically
		$rpc_py bdev_raid_remove_base_bdev "spare"
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs_operational - 1))
		$rpc_py bdev_raid_add_base_bdev $raid_bdev_name "spare"
		sleep 1
		verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"

		# Same as above but re-add through examine
		$rpc_py bdev_passthru_delete "spare"
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs_operational - 1))
		$rpc_py bdev_passthru_create -b "spare_delay" -p "spare"
		sleep 1
		verify_raid_bdev_process $raid_bdev_name "rebuild" "spare"

		# Stop the rebuild
		$rpc_py bdev_passthru_delete "spare"
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs_operational - 1))
		verify_raid_bdev_process $raid_bdev_name "none" "none"

		# Re-adding a base bdev that was replaced (no longer is a member of the array) should not be allowed
		$rpc_py bdev_passthru_delete ${base_bdevs[0]}
		$rpc_py bdev_passthru_create -b ${base_bdevs[0]}_malloc -p ${base_bdevs[0]}
		sleep 1
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs_operational - 1))
		verify_raid_bdev_process $raid_bdev_name "none" "none"
		NOT $rpc_py bdev_raid_add_base_bdev $raid_bdev_name ${base_bdevs[0]}
		sleep 1
		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs_operational - 1))
		verify_raid_bdev_process $raid_bdev_name "none" "none"
	fi

	killprocess $raid_pid

	return 0
}

function raid_io_error_test() {
	local raid_level=$1
	local num_base_bdevs=$2
	local error_io_type=$3
	local base_bdevs=($(for ((i = 1; i <= num_base_bdevs; i++)); do echo BaseBdev$i; done))
	local raid_bdev_name="raid_bdev1"
	local strip_size
	local create_arg
	local bdevperf_log
	local fail_per_s

	if [ $raid_level != "raid1" ]; then
		strip_size=64
		create_arg+=" -z $strip_size"
	else
		strip_size=0
	fi

	bdevperf_log=$(mktemp -p "$tmp_dir")

	"$rootdir/build/examples/bdevperf" -T $raid_bdev_name -t 60 -w randrw -M 50 -o 128k -q 1 -z -f -L bdev_raid > $bdevperf_log &
	raid_pid=$!
	waitforlisten $raid_pid

	# Create base bdevs
	for bdev in "${base_bdevs[@]}"; do
		$rpc_py bdev_malloc_create 32 $base_blocklen $base_malloc_params -b ${bdev}_malloc
		$rpc_py bdev_error_create ${bdev}_malloc
		$rpc_py bdev_passthru_create -b EE_${bdev}_malloc -p $bdev
	done

	# Create RAID bdev
	$rpc_py bdev_raid_create $create_arg -r $raid_level -b "'${base_bdevs[*]}'" -n $raid_bdev_name -s
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs

	# Start user I/O
	"$rootdir/examples/bdev/bdevperf/bdevperf.py" perform_tests &
	sleep 1

	# Inject an error
	$rpc_py bdev_error_inject_error EE_${base_bdevs[0]}_malloc $error_io_type failure

	local expected_num_base_bdevs
	if [[ $raid_level = "raid1" && $error_io_type = "write" ]]; then
		expected_num_base_bdevs=$((num_base_bdevs - 1))
	else
		expected_num_base_bdevs=$num_base_bdevs
	fi
	verify_raid_bdev_state $raid_bdev_name online $raid_level $strip_size $expected_num_base_bdevs

	$rpc_py bdev_raid_delete $raid_bdev_name

	killprocess $raid_pid

	# Check I/O failures reported by bdevperf
	# RAID levels with redundancy should handle the errors and not show any failures
	fail_per_s=$(grep -v Job $bdevperf_log | grep $raid_bdev_name | awk '{print $6}')
	if has_redundancy $raid_level; then
		[[ "$fail_per_s" = "0.00" ]]
	else
		[[ "$fail_per_s" != "0.00" ]]
	fi
}

function raid_resize_superblock_test() {
	local raid_level=$1

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	$rpc_py bdev_malloc_create -b malloc0 512 $base_blocklen

	$rpc_py bdev_passthru_create -b malloc0 -p pt0
	$rpc_py bdev_lvol_create_lvstore pt0 lvs0

	$rpc_py bdev_lvol_create -l lvs0 lvol0 64
	$rpc_py bdev_lvol_create -l lvs0 lvol1 64

	case $raid_level in
		0) $rpc_py bdev_raid_create -n Raid -r $raid_level -z 64 -b "'lvs0/lvol0 lvs0/lvol1'" -s ;;
		1) $rpc_py bdev_raid_create -n Raid -r $raid_level -b "'lvs0/lvol0 lvs0/lvol1'" -s ;;
	esac

	# Check size of base bdevs first
	(($(($($rpc_py bdev_get_bdevs -b lvs0/lvol0 | jq '.[].num_blocks') * 512 / 1048576)) == 64))
	(($(($($rpc_py bdev_get_bdevs -b lvs0/lvol1 | jq '.[].num_blocks') * 512 / 1048576)) == 64))

	# Check size of Raid bdev before resize
	case $raid_level in
		0) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 245760)) ;;
		1) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 122880)) ;;
	esac

	# Resize bdevs
	$rpc_py bdev_lvol_resize lvs0/lvol0 100
	$rpc_py bdev_lvol_resize lvs0/lvol1 100

	# Bdevs should be resized
	(($(($($rpc_py bdev_get_bdevs -b lvs0/lvol0 | jq '.[].num_blocks') * 512 / 1048576)) == 100))
	(($(($($rpc_py bdev_get_bdevs -b lvs0/lvol1 | jq '.[].num_blocks') * 512 / 1048576)) == 100))

	# Same with Raid bdevs
	case $raid_level in
		0) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 393216)) ;;
		1) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 196608)) ;;
	esac

	$rpc_py bdev_passthru_delete pt0
	$rpc_py bdev_passthru_create -b malloc0 -p pt0
	$rpc_py bdev_wait_for_examine

	# After the passthru bdev is re-created, the RAID bdev should start from
	# superblock and its size should be the same as after it was resized.
	case $raid_level in
		0) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 393216)) ;;
		1) (($($rpc_py bdev_get_bdevs -b Raid | jq '.[].num_blocks') == 196608)) ;;
	esac

	killprocess $raid_pid

	return 0
}

function raid_nvmf_uaf_test() {
	# Test for use-after-free crash in RAID1 bdev during NVMe-oF controller failures
	# Reproduces issue: https://github.com/spdk/spdk/issues/3703
	local num_base_bdevs=$1
	local max_iterations=$2
	# Test configuration - NQNs, ports, and naming prefixes
	local nqn_base="nqn.2023-01.test"
	local bdev_nqn_prefix="${nqn_base}:bdev_"
	local raid_nqn="${nqn_base}:raid"
	local bdev_name_prefix="bdev_"
	local lvs_name_prefix="bdev_lvs_"
	local lvol_name_prefix="bdev_lvol_"
	local controller_prefix="nvme"
	local disk_img_prefix="disk-img-"
	local base_port=4450
	local raid_port=4420
	local raid_name="raid1_test"
	local spdk_serial_base="SPDK00000000000001"
	local spdk_raid_serial="SPDK00000000000002"
	local tcp_addr="127.0.0.1"
	local raid_uaf_tmp_dir="$tmp_dir/raid_uaf_test"

	# Load NVMe module
	modprobe nvme-tcp
	for ((iteration = 1; iteration <= max_iterations; iteration++)); do
		echo "starting iteration $iteration of $max_iterations"

		# Start SPDK target with NVMe-oF support
		$rootdir/build/bin/spdk_tgt &
		local spdk_pid=$!
		waitforlisten $spdk_pid

		# Initialize NVMe-oF transport
		mkdir -p "$raid_uaf_tmp_dir"
		$rpc_py nvmf_create_transport -t TCP

		# Create base bdevs and export them via NVMe-oF
		for ((i = 0; i < num_base_bdevs; i++)); do
			local bdev_name="${bdev_name_prefix}$i"
			local lvs="${lvs_name_prefix}$i"
			local lvol="${lvol_name_prefix}$i"
			local nqn="${bdev_nqn_prefix}$i"
			local port=$((base_port + i))
			local controller="${controller_prefix}$i"
			local disk_img="$raid_uaf_tmp_dir/${disk_img_prefix}$i"

			# Create backing storage: AIO bdev -> LVOL store -> LVOL
			rm -rf $disk_img
			truncate -s 1024MB $disk_img
			$rpc_py bdev_aio_create $disk_img $bdev_name 512 --fallocate
			$rpc_py bdev_lvol_create_lvstore $bdev_name $lvs
			$rpc_py bdev_lvol_create -l $lvs $lvol 1024 -t

			# Export LVOL via NVMe-oF
			$rpc_py nvmf_create_subsystem $nqn -s "${spdk_serial_base}$i" -a
			$rpc_py nvmf_subsystem_add_ns -g "$(printf "1234567890abcdef%08x%08x" "$(date +%s)" $i)" $nqn $lvs/$lvol
			$rpc_py nvmf_subsystem_add_listener $nqn -t TCP -a $tcp_addr -s $port

			# Connect back to own NVMe-oF target to create local controller
			$rpc_py bdev_nvme_attach_controller -b $controller -t tcp -a $tcp_addr -n $nqn -s $port -f ipv4 \
				--ctrlr_loss_timeout_sec=2 --fast_io_fail_timeout_sec=2 --reconnect_delay_sec=1
		done

		# Build RAID1 array from attached NVMe controllers
		local bdevs=""
		for ((i = 0; i < num_base_bdevs; i++)); do
			bdevs+="${controller_prefix}${i}n1 "
		done
		$rpc_py bdev_raid_create -n $raid_name -r raid1 -b "'$bdevs'"

		# Export RAID1 bdev via NVMe-oF
		$rpc_py nvmf_create_subsystem $raid_nqn -s $spdk_raid_serial -a
		$rpc_py nvmf_subsystem_add_ns $raid_nqn $raid_name
		$rpc_py nvmf_subsystem_add_listener $raid_nqn -t TCP -a $tcp_addr -s $raid_port

		# Connect to RAID1 device from host side
		sleep 0.1
		local dev_before dev_after new_nvme_dev
		dev_before=$(printf "%s\n" /dev/nvme*n*)
		nvme connect -t tcp -n $raid_nqn -a $tcp_addr -s $raid_port --ctrl-loss-tmo=0
		sleep 0.1
		dev_after=$(printf "%s\n" /dev/nvme*n*)
		new_nvme_dev=$(comm -13 <(echo "$dev_before") <(echo "$dev_after"))

		# Mount filesystem and start I/O workload
		local data_dir="$raid_uaf_tmp_dir/data"
		mkdir -p $data_dir
		mkfs -t xfs -f $new_nvme_dev > /dev/null 2>&1
		mount -t xfs $new_nvme_dev $data_dir

		fio --name=raid_uaf_test \
			--directory=$data_dir \
			--sync=1 \
			--direct=1 \
			--size=1G \
			--rw=randwrite \
			--bs=4k \
			--ioengine=libaio \
			--iodepth=32 \
			--numjobs=10 \
			--timeout=60000 \
			--time_based=1 \
			--group_reporting \
			--output-format=terse \
			&

		local fio_pid=$!
		# Wait for some time to let I/O go through base bdevs
		sleep 4

		# Trigger UAF race condition: force controller disconnects during active I/O
		for ((i = 0; i < num_base_bdevs - 1; i++)); do
			local nqn="${bdev_nqn_prefix}$i"
			local port=$((base_port + i))
			$rpc_py nvmf_subsystem_remove_listener $nqn -t tcp -a $tcp_addr -s $port 2> /dev/null || true
			sleep 0.5
		done

		# Wait for UAF race condition: fast_io_fail_timeout_sec (2s) expires,
		# causing base bdev removal while NVMe module attempts I/O abort on
		# already-freed core channel pointers
		sleep 6

		killprocess $fio_pid 2> /dev/null || true
		umount -fl $new_nvme_dev 2> /dev/null || true
		nvme disconnect -n $raid_nqn 2> /dev/null || true
		rm -rf $raid_uaf_tmp_dir
		if ! kill -0 $spdk_pid 2> /dev/null; then
			echo "spdk_tgt crashed during iteration $iteration"
			return 1
		else
			killprocess $spdk_pid 2> /dev/null || true
		fi
	done

	return 0
}

function raid_resize_data_offset_test() {

	$rootdir/test/app/bdev_svc/bdev_svc -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid

	# Create three base bdevs with one null bdev to be replaced later
	$rpc_py bdev_malloc_create -b malloc0 64 $base_blocklen -o 16
	$rpc_py bdev_malloc_create -b malloc1 64 $base_blocklen -o 16
	$rpc_py bdev_null_create null0 64 $base_blocklen

	$rpc_py bdev_raid_create -n Raid -r 1 -b "'malloc0 malloc1 null0'" -s

	# Check data_offset
	(($($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[2].data_offset') == 2048))

	$rpc_py bdev_null_delete null0

	# Now null bdev is replaced with malloc, and optimal_io_boundary is changed to force
	# recalculation
	$rpc_py bdev_malloc_create -b malloc2 512 $base_blocklen -o 30
	$rpc_py bdev_raid_add_base_bdev Raid malloc2

	# Data offset is updated
	(($($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[2].data_offset') == 2070))

	killprocess $raid_pid

	return 0
}

mkdir -p "$tmp_dir"
trap 'cleanup; exit 1' EXIT

base_blocklen=512

run_test "raid_io_resource_dependence" raid_io_resource_dependence

run_test "raid1_resize_data_offset_test" raid_resize_data_offset_test

run_test "raid0_resize_superblock_test" raid_resize_superblock_test 0
run_test "raid1_resize_superblock_test" raid_resize_superblock_test 1

if [ $(uname -s) = Linux ] && modprobe -n nbd; then
	has_nbd=true
	modprobe nbd
	run_test "raid_unmap_function_test_raid0" raid_unmap_function_test raid0
	run_test "raid_unmap_function_test_concat" raid_unmap_function_test concat
	run_test "raid_unmap_function_test_raid1" raid_unmap_function_test raid1
fi

run_test "raid0_resize_test" raid_resize_test 0
run_test "raid1_resize_test" raid_resize_test 1

for n in {2..4}; do
	for level in raid0 concat raid1; do
		run_test "raid_state_function_test" raid_state_function_test $level $n false
		run_test "raid_state_function_test_sb" raid_state_function_test $level $n true
		run_test "raid_superblock_test" raid_superblock_test $level $n
		run_test "raid_read_error_test" raid_io_error_test $level $n read
		run_test "raid_write_error_test" raid_io_error_test $level $n write
	done
done

if [ "$has_nbd" = true ]; then
	for n in 2 4; do
		run_test "raid_rebuild_test" raid_rebuild_test raid1 $n false false true
		run_test "raid_rebuild_test_sb" raid_rebuild_test raid1 $n true false true
		run_test "raid_rebuild_test_io" raid_rebuild_test raid1 $n false true true
		run_test "raid_rebuild_test_sb_io" raid_rebuild_test raid1 $n true true true
	done
fi

for n in {3..4}; do
	run_test "raid5f_state_function_test" raid_state_function_test raid5f $n false
	run_test "raid5f_state_function_test_sb" raid_state_function_test raid5f $n true
	run_test "raid5f_superblock_test" raid_superblock_test raid5f $n
	if [ "$has_nbd" = true ]; then
		run_test "raid5f_rebuild_test" raid_rebuild_test raid5f $n false false true
		run_test "raid5f_rebuild_test_sb" raid_rebuild_test raid5f $n true false true
	fi
done

base_blocklen=4096

run_test "raid_state_function_test_sb_4k" raid_state_function_test raid1 2 true
run_test "raid_superblock_test_4k" raid_superblock_test raid1 2
if [ "$has_nbd" = true ]; then
	run_test "raid_rebuild_test_sb_4k" raid_rebuild_test raid1 2 true false true
fi

base_malloc_params="-m 32"
run_test "raid_state_function_test_sb_md_separate" raid_state_function_test raid1 2 true
run_test "raid_superblock_test_md_separate" raid_superblock_test raid1 2
if [ "$has_nbd" = true ]; then
	run_test "raid_rebuild_test_sb_md_separate" raid_rebuild_test raid1 2 true false true
fi

base_malloc_params="-m 32 -i"
run_test "raid_state_function_test_sb_md_interleaved" raid_state_function_test raid1 2 true
run_test "raid_superblock_test_md_interleaved" raid_superblock_test raid1 2
run_test "raid_rebuild_test_sb_md_interleaved" raid_rebuild_test raid1 2 true false false

run_test "raid_nvmf_uaf_test" raid_nvmf_uaf_test 10 10

trap - EXIT
cleanup
