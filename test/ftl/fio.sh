#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

device=$1
tests='drive-prep randw-verify-qd128-ext randw randr randrw'
uuid=$2

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 1
fi

export FTL_BDEV_CONF=$testdir/config/ftl.conf
export FTL_BDEV_NAME=nvme0

if [ -z "$uuid" ]; then
	$rootdir/scripts/gen_ftl.sh -a $device -n nvme0 -l 0-3 > $FTL_BDEV_CONF
else
	$rootdir/scripts/gen_ftl.sh -a $device -n nvme0 -l 0-3 -u $uuid > $FTL_BDEV_CONF
fi

for test in ${tests[@]}; do
	timing_enter $test
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $testdir/config/fio/$test.fio
	timing_exit $test
done

report_test_completion ftl_fio
