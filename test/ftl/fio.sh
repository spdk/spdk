#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

tests=(randw randw-verify randw-verify-j2 randw-verify-depth128)

rpc_py=$rootdir/scripts/rpc.py

fio_kill() {
	killprocess $svcpid
	rm -f $FTL_JSON_CONF
}

device=$1

if [[ $CONFIG_FIO_PLUGIN != y ]]; then
	echo "FIO not available"
	exit 1
fi

export FTL_BDEV_NAME=ftl0
export FTL_JSON_CONF=$testdir/config/ftl.json

trap "fio_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
bdev_create_zone nvme0n1
$rpc_py bdev_ftl_create -b ftl0 -d "$ZONE_DEV"

waitforbdev ftl0

(
	echo '{"subsystems": ['
	$rpc_py save_subsystem_config -n bdev
	echo ']}'
) > $FTL_JSON_CONF

killprocess $svcpid
trap - SIGINT SIGTERM EXIT

for test in "${tests[@]}"; do
	timing_enter $test
	fio_bdev $testdir/config/fio/$test.fio
	timing_exit $test
done

rm -f $FTL_JSON_CONF
