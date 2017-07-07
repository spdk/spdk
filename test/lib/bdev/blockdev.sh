#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

testdir=$(readlink -f $(dirname $0))

timing_enter blockdev

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter bounds
$testdir/bdevio/bdevio $testdir/bdev.conf
timing_exit bounds

if [ $(uname -s) = Linux ] && [ -f /usr/sbin/sgdisk ]; then
	echo "[Rpc]" >> $testdir/bdev.conf
	echo "  Enable Yes" >> $testdir/bdev.conf

	if [ ! -z "`grep "Nvme0" $testdir/bdev.conf`" ]; then
		modprobe nbd
		$testdir/nbd/nbd -c $testdir/bdev.conf -b Nvme0n1 -n /dev/nbd0 &
		nbd_pid=$!
		echo "Process nbd pid: $nbd_pid"
		waitforlisten $nbd_pid 5260
		#if return 1, it will trap, so do not need to consider this case
		waitforbdev Nvme0n1 $rootdir/scripts/rpc.py

		if [ -e /dev/nbd0 ]; then
			parted -s /dev/nbd0 mklabel gpt mkpart primary '0%' '50%' mkpart primary '50%' '100%'
			# change the partition type GUID to SPDK GUID value
			/usr/sbin/sgdisk -t 1:$SPDK_GPT_GUID /dev/nbd0
			/usr/sbin/sgdisk -t 2:$SPDK_GPT_GUID /dev/nbd0
		fi
		killprocess $nbd_pid

		# run nbd again to test get_bdevs
		$testdir/nbd/nbd -c $testdir/bdev.conf -b Nvme0n1 -n /dev/nbd0 &
		nbd_pid=$!
		waitforlisten $nbd_pid 5260
		waitforbdev Nvme0n1p1 $rootdir/scripts/rpc.py
		$rpc_py get_bdevs
		killprocess $nbd_pid
	fi
fi

timing_enter verify
$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 32 -s 4096 -w verify -t 1
timing_exit verify

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Use size 192KB which both exceeds typical 128KB max NVMe I/O
	#  size and will cross 128KB Intel DC P3700 stripe boundaries.
	timing_enter perf
	$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -w read -s 196608 -t 5
	timing_exit perf

	# Temporarily disabled - infinite loop
	#timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	#timing_exit reset

	timing_enter unmap
	$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 1 -w unmap -s 4096 -t 60
	timing_exit unmap
fi

rm -f $testdir/bdev.conf
timing_exit blockdev
