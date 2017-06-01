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

if [ $(uname -s) = Linux ]; then
	uuid=`grep SPDK_GPT_PART_TYPE_GUID $rootdir/lib/bdev/gpt/gpt.h | awk -F "(" '{ print $2}' | sed 's/)//g' | awk -F ", " '{ print $1 "-" $2 "-" $3 "-" $4 "-" $5}' | sed 's/0x//g'`
	echo "uuid=" $uuid

	echo " " >> $testdir/bdev.conf
	echo "[Gpt]" >> $testdir/bdev.conf
	echo "  Disable Yes" >> $testdir/bdev.conf
	
	#echo " " >> $testdir/bdev.conf
	#echo "[Rpc]" >> $testdir/bdev.conf
	#echo "  Enable Yes" >> $testdir/bdev.conf
	#$testdir/nbd/nbd -c $testdir/bdev.conf -b Malloc0 -n /dev/nbd0 &
	#nbd_pid=$!
        #echo "Process nbd pid: $nbd_pid"
	#waitforlisten $nbd_pid 5260	

	#if [ -f /dev/nbd0 ]; then
		echo "enter here"
		disk=/home/ziyeyang/spdk_external/spdk/aio-test.img
		parted -s $disk mklabel gpt mkpart primary '0%' '50%' mkpart primary '50%' '100%'
		#change the GUID to SPDK GUID value
		sgdisk -u 1:$uuid $disk
		#sgdisk -u 2:$uuid $disk
	#fi

	#kill $nbd_pid

fi

timing_enter verify
$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 2 -s 4096 -w verify -t 1 -o all
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
