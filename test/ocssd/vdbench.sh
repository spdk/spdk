#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh
device=$2

declare -A test_suite
test_suite['short']='randrw_immediate_validate randrw_validate'
test_suite['long']='randrw_immediate_validate_nightly randrw_validate_nightly'

if ! hash vdbench; then
	echo "vdbench not available"
	exit 0
fi

function vdbench_kill() {
	killprocess $svcpid
	rmmod nbd || true
}

suite=$(echo $1 | sed 's/^--//g')
if [ -z "${test_suite[$suite]}" ]; then
	echo "Unknown test suite '$suite'"
	exit 1
fi

$testdir/generate_configs.sh -a $device -n nvme0 -l 0-3 -m 3
$rootdir/test/app/bdev_svc/bdev_svc -c $testdir/config/ocssd.conf &
svcpid=$!

trap "vdbench_kill; exit 1" SIGINT SIGTERM EXIT

# Load the nbd driver
modprobe nbd
$testdir/start_nbd.py

for test in ${test_suite[$suite]}; do
	vdbench -f $testdir/config/vdbench/${test}.ini
done

report_test_completion occsd_vdbench

trap - SIGINT SIGTERM EXIT
vdbench_kill
