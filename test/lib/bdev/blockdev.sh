#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/scripts/autotest_common.sh

timing_enter bdev

# Create a file to be used as an AIO backend
dd if=/dev/zero of=/tmp/aiofile bs=2048 count=5000

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter bounds
$testdir/bdevio/bdevio $testdir/bdev.conf
timing_exit bounds

timing_enter nbd
if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir
fi
timing_exit nbd

timing_enter bdev_svc
bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf | jq -r '.[] | select(.bdev_opened_for_write == false)')
timing_exit bdev_svc

if [ -d /usr/src/fio ] && [ $SPDK_RUN_ASAN -eq 0 ]; then
	timing_enter fio

	timing_enter fio_rw_verify
	# Generate the fio config file given the list of all unclaimed bdevs
	fio_config_gen $testdir/bdev.fio verify
	for b in $(echo $bdevs | jq -r '.name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio
	fi

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_rw_verify

	timing_enter fio_trim
	# Generate the fio config file given the list of all unclaimed bdevs that support unmap
	fio_config_gen $testdir/bdev.fio trim
	for b in $(echo $bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
		fio_config_add_job $testdir/bdev.fio $b
	done

	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio
	fi

	rm -f *.state
	rm -f $testdir/bdev.fio
	timing_exit fio_trim

	timing_exit fio
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Temporarily disabled - infinite loop
	timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	timing_exit reset
fi


if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir reset
fi

rm -f /tmp/aiofile
rm -f $testdir/bdev.conf
timing_exit bdev
