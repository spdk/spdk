#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

declare -A suite
suite['basic']='randw-verify randw-verify-j2 randw-verify-depth128'
suite['extended']='drive-prep randw-verify-qd128-ext randw randr randrw'

device=$1
tests=${suite[$2]}
uuid=$3

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 1
fi

if [ -z "$tests" ]; then
	echo "Invalid test suite '$2'"
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
	fio_bdev $testdir/config/fio/$test.fio
	timing_exit $test
done

report_test_completion ftl_fio
