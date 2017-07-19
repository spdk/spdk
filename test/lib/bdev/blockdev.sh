#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/scripts/autotest_common.sh

timing_enter bdev

rpc_py=$rootdir/scripts/rpc.py

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
# Start the bdev service to query for the list of available
# bdevs.
$rootdir/test/app/bdev_svc/bdev_svc -i 0 -c $testdir/bdev.conf &
stubpid=$!
while ! [ -e /var/run/spdk_bdev0 ]; do
	sleep 1
done
# Get all of the bdevs that aren't opened for write
bdevs=$($rpc_py get_bdevs | jq -r '.[] | select(.bdev_opened_for_write == false) | .name')
# Shut down the bdev service
kill $stubpid
wait $stubpid
rm -f /var/run/spdk_bdev0
timing_exit bdev_svc

if [ -d /usr/src/fio ] && [ $SPDK_RUN_ASAN -eq 0 ]; then
	timing_enter fio

	# Generate the fio config file given the list of all unclaimed bdevs
	cp $testdir/bdev.fio.in $testdir/bdev.fio
	for b in $bdevs; do
		echo ""
		echo "[job_$b]" >> $testdir/bdev.fio
		echo "filename=$b" >> $testdir/bdev.fio
	done

	if [ $RUN_NIGHTLY -eq 0 ]; then
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=8 --bs=4k --runtime=10 $testdir/bdev.fio
	else
		# Use size 192KB which both exceeds typical 128KB max NVMe I/O
		#  size and will cross 128KB Intel DC P3700 stripe boundaries.
		LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk --spdk_conf=./test/lib/bdev/bdev.conf --iodepth=128 --bs=192k --runtime=100 $testdir/bdev.fio
	fi

	rm -f $testdir/bdev.fio
	timing_exit fio
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Temporarily disabled - infinite loop
	#timing_enter reset
	#$testdir/bdevperf/bdevperf -c $testdir/bdev.conf -q 16 -w reset -s 4096 -t 60
	#timing_exit reset
fi

rm -f $testdir/bdev.conf
timing_exit bdev
