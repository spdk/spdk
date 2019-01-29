#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py=$rootdir/scripts/rpc.py

source $rootdir/test/common/autotest_common.sh

vendor_id='0x1d1d'
device_id='0x1f1f'
device=$(lspci -d ${vendor_id}:${device_id} | cut -d' ' -f 1)

if [ -z "$device" ]; then
	echo "Could not find FTL device. Tests skipped."
	exit 0
fi

timing_enter ftl
timing_enter fio

#run_test suite $testdir/fio.sh $device basic

timing_exit fio

timing_enter restore
#run_test suite $testdir/restore.sh $device
timing_exit restore

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	$rootdir/test/app/bdev_svc/bdev_svc &
	bdev_svc_pid=$!

	trap "killprocess $bdev_svc_pid; exit 1" SIGINT SIGTERM EXIT

	waitforlisten $bdev_svc_pid
	uuid=$($rpc_py construct_ftl_bdev -b nvme0 -a $device -l 0-3 | jq -r '.uuid')
	killprocess $bdev_svc_pid

	trap - SIGINT SIGTERM EXIT

	timing_enter fio_extended
	run_test suite $testdir/fio.sh $device extended $uuid
	timing_exit fio_extended
fi

timing_exit ftl
