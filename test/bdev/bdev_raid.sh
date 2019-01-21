#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py="$rootdir/scripts/rpc.py"
tmp_file=/tmp/raidrandtest

source $rootdir/test/common/autotest_common.sh
source $testdir/nbd_common.sh

function raid_unmap_data_verify() {
	if hash blkdiscard; then
		local nbd=$1
		local rpc_server=$2
		local blksize=$(lsblk -o  LOG-SEC $nbd | grep -v LOG-SEC | cut -d ' ' -f 5)
		local rw_blk_num=4096
		local rw_len=$((blksize * rw_blk_num))
		local unmap_blk_offs=(0   1028 321)
		local unmap_blk_nums=(128 2035 456)
		local unmap_off
		local unmap_len

		# data write
		dd if=/dev/urandom of=$tmp_file bs=$blksize count=$rw_blk_num
		dd if=$tmp_file of=$nbd bs=$blksize count=$rw_blk_num oflag=direct
		blockdev --flushbufs $nbd

		# confirm random data is written correctly in raid0 device
		cmp -b -n $rw_len $tmp_file $nbd

		for (( i=0; i<${#unmap_blk_offs[@]}; i++ )); do
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
	if [ ! -z $raid_pid ]; then
		killprocess $raid_pid
	fi

	rm -f $testdir/bdev.conf
	rm -f $tmp_file
	print_backtrace
	exit 1
}

function raid_function_test() {
	if [ $(uname -s) = Linux ] && modprobe -n nbd; then
		local rpc_server=/var/tmp/spdk-raid.sock
		local conf=$1
		local nbd=/dev/nbd0
		local raid_bdev

		if [ ! -e $conf ]; then
			return 1
		fi

		modprobe nbd
		$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -i 0 -c ${conf} -L bdev_raid &
		raid_pid=$!
		echo "Process raid pid: $raid_pid"
		waitforlisten $raid_pid $rpc_server

		raid_bdev=$($rootdir/scripts/rpc.py -s $rpc_server get_raid_bdevs online | cut -d ' ' -f 1)
		if [ $raid_bdev = "" ]; then
			echo "No raid0 device in SPDK app"
			return 1
		fi

		nbd_start_disks $rpc_server $raid_bdev $nbd
		count=$(nbd_get_count $rpc_server)
		if [ $count -ne 1 ]; then
			return -1
		fi

		raid_unmap_data_verify $nbd $rpc_server

		nbd_stop_disks $rpc_server $nbd
		count=$(nbd_get_count $rpc_server)
		if [ $count -ne 0 ]; then
			return -1
		fi

		killprocess $raid_pid
	fi

	return 0
}

timing_enter bdev_raid
trap 'on_error_exit;' ERR

cp $testdir/bdev.conf.in $testdir/bdev.conf
raid_function_test $testdir/bdev.conf

rm -f $testdir/bdev.conf
rm -f $tmp_file
report_test_completion "bdev_raid"
timing_exit bdev_raid
