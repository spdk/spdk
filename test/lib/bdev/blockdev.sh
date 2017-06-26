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
	$rootdir/scripts/setup.sh reset
        sleep 5

        bdfs=$(lspci -mm -n -D | grep 0108 | tr -d '"' | awk -F " " '{print $1}')

        # partition the nvme disk into 2 partitions.
        for bdf in $bdfs; do
                name=`ls /sys/bus/pci/devices/$bdf/nvme`
                parted -s /dev/"$name"n1 mklabel gpt mkpart primary '0%' '50%' mkpart primary '50%' '100%'
                #change the GUID to SPDK GUID value
                /usr/sbin/sgdisk -u 1:$SPDK_GPT_UUID /dev/"$name"n1
                /usr/sbin/sgdisk -u 2:$SPDK_GPT_UUID /dev/"$name"n1
        done

        $rootdir/scripts/setup.sh
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
