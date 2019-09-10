#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

function error_exit() {
	set +e
	echo "Error on $1 - $2"
	$rpc_py bdev_lvol_delete_lvstore -l lvs_0
	killprocess $spdk_tgt_pid
	./scripts/setup.sh
	print_backtrace
	exit 1
}

function run_spdk_tgt() {
        $rootdir/app/spdk_tgt/spdk_tgt &
        spdk_tgt_pid=$!
        waitforlisten $spdk_tgt_pid
	$rootdir/scripts/gen_nvme.sh "--json" | $rootdir/scripts/rpc.py load_subsystem_config
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

$rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs_0 --clear-method=none
$rpc_py bdev_lvol_create lvol_0 1000 --lvs-name lvs_0 --clear-method=none

killprocess $spdk_tgt_pid
./scripts/setup.sh reset
dd if=/dev/nvme0n1 of=testfile bs=1M count=50
hex_dump=$(hexdump -n 1024 testfile)

./scripts/setup.sh
run_spdk_tgt

$rpc_py bdev_lvol_delete lvs_0/lvol_0

killprocess $spdk_tgt_pid
./scripts/setup.sh reset
dd if=/dev/nvme0n1 of=testfile bs=1M count=50
hex_dump_lbd=$(hexdump -n 1024 testfile)

if [[ ! hex_dump == hex_dump_lbd ]]; then
	echo "Clear method for lvol bdev does not function"
	error_exit
fi

./scripts/setup.sh
run_spdk_tgt
$rpc_py bdev_lvol_delete_lvstore -l lvs_0

killprocess $spdk_tgt_pid
./scripts/setup.sh reset
hex_dump_lvs=$(hexdump -n 1024 testfile)

if [[ ! hex_dump == hex_dump_lvs ]]; then
        echo "Clear method for lvol store does not function"
        error_exit
fi

./scripts/setup.sh
