#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/scripts/autotest_common.sh

timing_enter bdev

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter nbd
if [ $(uname -s) = Linux ] && hash sgdisk; then
	echo "[Rpc]" >> $testdir/bdev.conf
	echo "  Enable Yes" >> $testdir/bdev.conf
	echo "[Gpt]" >> $testdir/bdev.conf
	echo "  Disable Yes" >> $testdir/bdev.conf

	if grep -q Nvme0 $testdir/bdev.conf; then
		modprobe nbd
		$testdir/nbd/nbd -c $testdir/bdev.conf -b Nvme0n1 -n /dev/nbd0 &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid 5260
		#if return 1, it will trap, so do not need to consider this case
		waitforbdev Nvme0n1 $rootdir/scripts/rpc.py

		if [ -e /dev/nbd0 ]; then
			parted -s /dev/nbd0 mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'
			# change the partition type GUID to SPDK GUID value
			sgdisk -t 1:$SPDK_GPT_GUID /dev/nbd0
			sgdisk -t 2:$SPDK_GPT_GUID /dev/nbd0
		fi
		killprocess $nbd_pid

		# enable the gpt module and run nbd again to test get_bdevs and
		# bind nbd to the new gpt partition bdev
		sed -i'' '/Disable/d' $testdir/bdev.conf
		$testdir/nbd/nbd -c $testdir/bdev.conf -b Nvme0n1p1 -n /dev/nbd0 &
		nbd_pid=$!
		waitforlisten $nbd_pid 5260
		waitforbdev Nvme0n1p1 $rootdir/scripts/rpc.py
		$rpc_py get_bdevs
		killprocess $nbd_pid
	fi
fi
timing_exit nbd

if [ $SPDK_RUN_ASAN -eq 0 ]; then
	timing_enter fio
	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /home/bwalker/src/fio/fio $testdir/bdev.fio --iodepth=8 --bs=4k --runtime=10
	else
		LD_PRELOAD=$plugindir/fio_plugin /home/bwalker/src/fio/fio $testdir/bdev.fio --iodepth=128 --bs=192k --runtime=60
	fi
	timing_exit fio
fi

rm -f $testdir/bdev.conf
timing_exit bdev
