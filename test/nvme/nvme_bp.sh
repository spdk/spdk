#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [ $(uname) = Linux ]; then
	$rootdir/scripts/setup.sh
fi

run_test "nvme_boot_partition" $testdir/boot_partition/boot_partition
