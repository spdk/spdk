#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_server=/var/tmp/spdk-raid.sock
rpc_py="$rootdir/scripts/rpc.py -s $rpc_server"
tmp_file=$SPDK_TEST_STORAGE/raidrandtest

source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

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
	rm -rf $testdir/rpcs.txt

	cat <<- EOL >> $testdir/rpcs.txt
		bdev_malloc_create 32 512 -b Base_1
		bdev_malloc_create 32 512 -b Base_2
		bdev_raid_create -z 64 -r 0 -b "Base_1 Base_2" -n raid0
	EOL
	$rpc_py < $testdir/rpcs.txt

	rm -rf $testdir/rpcs.txt
}

function raid_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local nbd=/dev/nbd0
		local raid_bdev

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -L bdev_raid &
		raid_pid=$!
		echo "Process raid pid: $raid_pid"
		waitforlisten $raid_pid $rpc_server

		configure_raid_bdev
		raid_bdev=$($rpc_py bdev_raid_get_bdevs online | cut -d ' ' -f 1)
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

trap 'on_error_exit;' ERR

raid_function_test

rm -f $tmp_file
