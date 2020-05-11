#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

declare -A suite
suite['basic']='randw-verify randw-verify-j2 randw-verify-depth128'
suite['extended']='drive-prep randw-verify-qd128-ext randw randr randrw'

rpc_py=$rootdir/scripts/rpc.py

fio_kill() {
	killprocess $svcpid
	rm -f $FTL_JSON_CONF
}

device=$1
tests=${suite[$2]}
uuid=$3

if [[ $CONFIG_FIO_PLUGIN != y ]]; then
	echo "FIO not available"
	exit 1
fi

if [ -z "$tests" ]; then
	echo "Invalid test suite '$2'"
	exit 1
fi

export FTL_BDEV_NAME=ftl0
export FTL_JSON_CONF=$testdir/config/ftl.json

trap "fio_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1

if [ -z "$uuid" ]; then
	$rpc_py bdev_ftl_create -b ftl0 -d nvme0n1
else
	$rpc_py bdev_ftl_create -b ftl0 -d nvme0n1 -u $uuid
fi

waitforbdev ftl0

(
	echo '{"subsystems": ['
	$rpc_py save_subsystem_config -n bdev
	echo ']}'
) > $FTL_JSON_CONF

killprocess $svcpid
trap - SIGINT SIGTERM EXIT

for test in ${tests}; do
	timing_enter $test
	fio_bdev $testdir/config/fio/$test.fio
	timing_exit $test
done

rm -f $FTL_JSON_CONF
