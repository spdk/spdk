#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

declare -A suite
suite['basic']='randw-verify randw-verify-j2 randw-verify-depth128'
suite['extended']='drive-prep randw-verify-qd128-ext randw randr randrw'

rpc_py=$rootdir/scripts/rpc.py

fio_kill() {
	killprocess $svcpid
}

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

export FTL_BDEV_NAME=/dev/nbd0

trap "fio_kill; exit 1" SIGINT SIGTERM EXIT

$rootdir/app/spdk_tgt/spdk_tgt & svcpid=$!
waitforlisten $svcpid

$rpc_py construct_nvme_bdev -b nvme0 -a $device -m ocssd -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1

if [ -z "$uuid" ]; then
	$rpc_py construct_ftl_bdev -b ftl0 -d nvme0n1
else
	$rpc_py construct_ftl_bdev -b ftl0 -d nvme0n1 -u $uuid
fi

$rpc_py start_nbd_disk ftl0 /dev/nbd0

for test in ${tests[@]}; do
	timing_enter $test
	/usr/src/fio/fio $testdir/config/fio/$test.fio
	timing_exit $test
done

report_test_completion ftl_fio
