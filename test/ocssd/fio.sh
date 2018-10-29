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
	exit 0
fi

$testdir/generate_configs.sh -a $device -n nvme0 -l 0-3 -m 1

for test in ${tests[@]}; do
	timing_enter $test
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $testdir/config/fio/$test.fio
	timing_exit $test
done

report_test_completion occsd_fio
