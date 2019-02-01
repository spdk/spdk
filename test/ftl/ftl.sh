#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

function ftl_kill() {
	rm -f $testdir/.testfile_*
}

vendor_id='0x1d1d'
device_id='0x1f1f'
device=$(lspci -d ${vendor_id}:${device_id} | cut -d' ' -f 1)

if [ -z "$device" ]; then
	echo "Could not find FTL device. Tests skipped."
	exit 0
fi

trap "ftl_kill; exit 1" SIGINT SIGTERM EXIT

timing_enter ftl
timing_enter fio

run_test suite $testdir/fio.sh $device

timing_exit fio

timing_enter restore
run_test suite $testdir/restore.sh $device $uuid
timing_exit restore

timing_exit ftl

trap - SIGINT SIGTERM EXIT
ftl_kill
