#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

tests=(randw-verify randw-verify-j2 randw-verify-depth128)
plugindir=$rootdir/examples/bdev/fio_plugin
device=$1

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 1
fi

$rootdir/scripts/gen_ftl.sh -a $device -n nvme0 -l 0-3

for test in ${tests[@]}; do
	timing_enter $test
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $testdir/config/fio/$test.fio
	timing_exit $test
done

report_test_completion ftl_fio
