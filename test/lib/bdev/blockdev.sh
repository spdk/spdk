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

	modprobe nbd
	$testdir/nbd/nbd -c $testdir/bdev.conf -b Nvme0n1 -n /dev/nbd0 &
	nbd_pid=$!
	echo "Process nbd pid: $nbd_pid"
	waitforlisten $nbd_pid 5260

	# Make sure nbd process still run. The reaon is that:
	# nbd program may exist by calling spdk_app_stop due to some issues in
	# nbd_start function. And we can not catch the abnormal exit.
	sleep 5

	if [ `ps aux | awk -F " " '{print $2}' | grep $nbd_pid` ]; then
		if [ -e /dev/nbd0 ]; then
			parted -s /dev/nbd0 mklabel gpt mkpart primary '0%' '50%' mkpart primary '50%' '100%'
			#change the GUID to SPDK GUID value
			/usr/sbin/sgdisk -u 1:$SPDK_GPT_UUID /dev/nbd0
			/usr/sbin/sgdisk -u 2:$SPDK_GPT_UUID /dev/nbd0
		fi

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
