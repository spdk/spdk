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
	local nbd=/dev/nbd0
	local raid_bdev

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

	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -L bdev_raid &
	raid_pid=$!
	echo "Process raid pid: $raid_pid"
	waitforlisten $raid_pid $rpc_server

	# Step1: create a RAID bdev with no base bdevs
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "${base_bdevs[*]}" -n $raid_bdev_name
	verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
	$rpc_py bdev_raid_delete $raid_bdev_name

	# Step2: create one base bdev and add to the RAID bdev
	# Expect state: CONFIGURING
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "${base_bdevs[*]}" -n $raid_bdev_name
	$rpc_py bdev_malloc_create 32 512 -b ${base_bdevs[0]}
	waitforbdev ${base_bdevs[0]}
	verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
	$rpc_py bdev_raid_delete $raid_bdev_name

	if [ $superblock = true ]; then
		# recreate the bdev to remove superblock
		$rpc_py bdev_malloc_delete ${base_bdevs[0]}
		$rpc_py bdev_malloc_create 32 512 -b ${base_bdevs[0]}
		waitforbdev ${base_bdevs[0]}
	fi

	# Step3: create remaining base bdevs and add to the RAID bdev
	# Expect state: ONLINE
	$rpc_py bdev_raid_create $strip_size_create_arg $superblock_create_arg -r $raid_level -b "${base_bdevs[*]}" -n $raid_bdev_name
	for ((i = 1; i < num_base_bdevs; i++)); do
		verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs
		$rpc_py bdev_malloc_create 32 512 -b ${base_bdevs[$i]}
		waitforbdev ${base_bdevs[$i]}
	done
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs

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

	"$rootdir/test/app/bdev_svc/bdev_svc" -r $rpc_server -L bdev_raid &
	raid_pid=$!
	waitforlisten $raid_pid $rpc_server

	# Create base bdevs
	for ((i = 1; i <= num_base_bdevs; i++)); do
		local bdev_malloc="malloc$i"
		local bdev_pt="pt$i"
		local bdev_pt_uuid="00000000-0000-0000-0000-00000000000$i"

		base_bdevs_malloc+=($bdev_malloc)
		base_bdevs_pt+=($bdev_pt)
		base_bdevs_pt_uuid+=($bdev_pt_uuid)

		$rpc_py bdev_malloc_create 32 512 -b $bdev_malloc
		$rpc_py bdev_passthru_create -b $bdev_malloc -p $bdev_pt -u $bdev_pt_uuid
	done

	# Create RAID bdev with superblock
	$rpc_py bdev_raid_create $strip_size_create_arg -r $raid_level -b "${base_bdevs_pt[*]}" -n $raid_bdev_name -s
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs

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
	NOT $rpc_py bdev_raid_create $strip_size_create_arg -r $raid_level -b "${base_bdevs_malloc[*]}" -n $raid_bdev_name

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

		if [ $num_base_bdevs -gt 2 ]; then
			# Stop the RAID bdev
			$rpc_py bdev_raid_delete $raid_bdev_name
			raid_bdev=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[]')
			if [ -n "$raid_bdev" ]; then
				return 1
			fi

			# Re-add first base bdev
			# This is the "failed" device and contains the "old" version of the superblock
			$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[0]} -p ${base_bdevs_pt[0]} -u ${base_bdevs_pt_uuid[0]}

			# Check if the RAID bdev is in configuring state
			verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $num_base_bdevs

			# Delete remaining base bdevs
			for ((i = 1; i < num_base_bdevs; i++)); do
				$rpc_py bdev_passthru_delete ${base_bdevs_pt[$i]}
			done

			# Re-add the last base bdev
			i=$((num_base_bdevs - 1))
			$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}

			# Check if the RAID bdev is in configuring state
			# This should use the newer superblock version and have n-1 online base bdevs
			verify_raid_bdev_state $raid_bdev_name "configuring" $raid_level $strip_size $((num_base_bdevs - 1))

			# Re-add remaining base bdevs
			for ((i = 1; i < num_base_bdevs - 1; i++)); do
				$rpc_py bdev_passthru_create -b ${base_bdevs_malloc[$i]} -p ${base_bdevs_pt[$i]} -u ${base_bdevs_pt_uuid[$i]}
			done

			# Check if the RAID bdev is in online state (degraded)
			verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $((num_base_bdevs - 1))
		fi

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

	"$rootdir/build/examples/bdevperf" -r $rpc_server -T $raid_bdev_name -t 60 -w randrw -M 50 -o 3M -q 2 -U -z -L bdev_raid &
	raid_pid=$!
	waitforlisten $raid_pid $rpc_server

	# Create base bdevs
	for bdev in "${base_bdevs[@]}"; do
		if [ $superblock = true ]; then
			$rpc_py bdev_malloc_create 32 512 -b ${bdev}_malloc
			$rpc_py bdev_passthru_create -b ${bdev}_malloc -p $bdev
		else
			$rpc_py bdev_malloc_create 32 512 -b $bdev
		fi
	done

	# Create spare bdev
	$rpc_py bdev_malloc_create 32 512 -b "spare_malloc"
	$rpc_py bdev_delay_create -b "spare_malloc" -d "spare_delay" -r 0 -t 0 -w 100000 -n 100000
	$rpc_py bdev_passthru_create -b "spare_delay" -p "spare"

	# Create RAID bdev
	$rpc_py bdev_raid_create $create_arg -r $raid_level -b "${base_bdevs[*]}" -n $raid_bdev_name
	verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs

	# Get RAID bdev's size
	raid_bdev_size=$($rpc_py bdev_get_bdevs -b $raid_bdev_name | jq -r '.[].num_blocks')

	# Get base bdev's data offset
	data_offset=$($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[0].data_offset')

	if [ $background_io = true ]; then
		# Start user I/O
		"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s $rpc_server perform_tests &
	else
		local write_unit_size

		# Write random data to the RAID bdev
		nbd_start_disks $rpc_server $raid_bdev_name /dev/nbd0
		if [ $raid_level = "raid5f" ]; then
			write_unit_size=$((strip_size * 2 * (num_base_bdevs - 1)))
			echo $((512 * write_unit_size / 1024)) > /sys/block/nbd0/queue/max_sectors_kb
		else
			write_unit_size=1
		fi
		dd if=/dev/urandom of=/dev/nbd0 bs=$((512 * write_unit_size)) count=$((raid_bdev_size / write_unit_size)) oflag=direct
		nbd_stop_disks $rpc_server /dev/nbd0
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

	if [ $background_io = true ]; then
		# Compare data on the rebuilt and other base bdevs
		nbd_start_disks $rpc_server "spare" "/dev/nbd0"
		for bdev in "${base_bdevs[@]:1}"; do
			if [ -z "$bdev" ]; then
				continue
			fi
			nbd_start_disks $rpc_server $bdev "/dev/nbd1"
			cmp -i $((data_offset * 512)) /dev/nbd0 /dev/nbd1
			nbd_stop_disks $rpc_server "/dev/nbd1"
		done
		nbd_stop_disks $rpc_server "/dev/nbd0"
	else
		# Compare data on the removed and rebuilt base bdevs
		nbd_start_disks $rpc_server "${base_bdevs[0]} spare" "/dev/nbd0 /dev/nbd1"
		cmp -i $((data_offset * 512)) /dev/nbd0 /dev/nbd1
		nbd_stop_disks $rpc_server "/dev/nbd0 /dev/nbd1"
	fi

	if [ $superblock = true ]; then
		# Remove the passthru base bdevs, then re-add them to assemble the raid bdev again
		for bdev in "${base_bdevs[@]}"; do
			if [ -z "$bdev" ]; then
				continue
			fi
			$rpc_py bdev_passthru_delete $bdev
			$rpc_py bdev_passthru_create -b ${bdev}_malloc -p $bdev
		done
		$rpc_py bdev_passthru_delete "spare"
		$rpc_py bdev_passthru_create -b "spare_delay" -p "spare"

		verify_raid_bdev_state $raid_bdev_name "online" $raid_level $strip_size $num_base_bdevs_operational
		verify_raid_bdev_process $raid_bdev_name "none" "none"
		[[ $($rpc_py bdev_raid_get_bdevs all | jq -r '.[].base_bdevs_list[0].name') == "spare" ]]
	fi

	killprocess $raid_pid

	return 0
}

trap 'on_error_exit;' ERR

if [ $(uname -s) = Linux ] && modprobe -n nbd; then
	has_nbd=true
	modprobe nbd
	run_test "raid_function_test_raid0" raid_function_test raid0
	run_test "raid_function_test_concat" raid_function_test concat
fi

run_test "raid0_resize_test" raid0_resize_test

for n in {2..4}; do
	for level in raid0 concat raid1; do
		run_test "raid_state_function_test" raid_state_function_test $level $n false
		run_test "raid_state_function_test_sb" raid_state_function_test $level $n true
		run_test "raid_superblock_test" raid_superblock_test $level $n
	done
done

if [ "$has_nbd" = true ]; then
	for n in 2 4; do
		run_test "raid_rebuild_test" raid_rebuild_test raid1 $n false false
		run_test "raid_rebuild_test_sb" raid_rebuild_test raid1 $n true false
		run_test "raid_rebuild_test_io" raid_rebuild_test raid1 $n false true
		run_test "raid_rebuild_test_sb_io" raid_rebuild_test raid1 $n true true
	done
fi

if [ "$CONFIG_RAID5F" == y ]; then
	for n in {3..4}; do
		run_test "raid5f_state_function_test" raid_state_function_test raid5f $n false
		run_test "raid5f_state_function_test_sb" raid_state_function_test raid5f $n true
		run_test "raid5f_superblock_test" raid_superblock_test raid5f $n
		if [ "$has_nbd" = true ]; then
			run_test "raid5f_rebuild_test" raid_rebuild_test raid5f $n false false
			run_test "raid5f_rebuild_test_sb" raid_rebuild_test raid5f $n true false
		fi
	done
fi

rm -f $tmp_file
