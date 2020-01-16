#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

declare -A suite
suite['basic']='randw-verify randw-verify-j2 randw-verify-depth128'
suite['extended']='drive-prep randw-verify-qd128-ext randw randr randrw'

rpc_py=$rootdir/scripts/rpc.py
ftl_bdev_conf=$testdir/config/ftl.conf
gen_ftl_nvme_conf > $ftl_bdev_conf

fio_kill() {
	$rpc_py stop_nbd_disk /dev/nbd0
	rmmod nbd || true
	killprocess $svcpid
	rm -f $ftl_bdev_conf
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

$rootdir/app/spdk_tgt/spdk_tgt -c $ftl_bdev_conf & svcpid=$!
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1

if [ -z "$uuid" ]; then
	$rpc_py bdev_ftl_create -b ftl0 -d nvme0n1
else
	$rpc_py bdev_ftl_create -b ftl0 -d nvme0n1 -u $uuid
fi

modprobe nbd
$rpc_py start_nbd_disk ftl0 /dev/nbd0
waitfornbd nbd0

for test in ${tests}; do
	timing_enter $test
	fio_bdev $testdir/config/fio/$test.fio
	timing_exit $test
done

trap - SIGINT SIGTERM EXIT
fio_kill
