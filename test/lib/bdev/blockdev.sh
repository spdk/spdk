#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

testdir=$(readlink -f $(dirname $0))

timing_enter blockdev

rpc_py=$rootdir/scripts/rpc.py

cp $testdir/bdev.conf.in $testdir/bdev.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/bdev.conf

timing_enter bounds
$testdir/bdevio/bdevio $testdir/bdev.conf
timing_exit bounds

if grep -q Nvme0 $testdir/bdev.conf; then
	part_dev_by_gpt $testdir/bdev.conf Nvme0n1 $rootdir
fi

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
# For now, just print the list of bdevs. This will be used in later tests.
echo $bdevs
# Shut down the bdev service
kill $stubpid
wait $stubpid
rm -f /var/run/spdk_bdev0
timing_exit bdev_svc

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
