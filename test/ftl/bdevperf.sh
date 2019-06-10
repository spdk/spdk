#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

tests=('-q 1 -w randwrite -t 4 -o 69632' '-q 128 -w randwrite -t 4 -o 4096' '-q 128 -w verify -t 4 -o 4096')
device=$1
ftl_bdev_conf=$testdir/config/ftl.conf

$rootdir/scripts/gen_ftl.sh -a $device -n nvme0 -l 0-3 > $ftl_bdev_conf

for (( i=0; i<${#tests[@]}; i++ )) do
	timing_enter "${tests[$i]}"
	$rootdir/test/bdev/bdevperf/bdevperf -c $ftl_bdev_conf ${tests[$i]}
	timing_exit "${tests[$i]}"
done

report_test_completion ftl_bdevperf
