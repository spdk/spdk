#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"

# The OPAL CI tests is only used for P4510 devices.
mapfile -t bdfs < <(get_nvme_bdfs_by_id 0x0a54)
if [[ -z ${bdfs[0]} ]]; then
	echo "No P4510 device found, exit the tests"
	exit 1
fi

bdf=${bdfs[0]}

function opal_revert_and_init() {
	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!
	trap 'killprocess $spdk_tgt_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_tgt_pid

	$rootdir/scripts/rpc.py bdev_nvme_attach_controller -b "nvme0" -t "pcie" -a ${bdf}
	# Ignore if this fails.
	$rootdir/scripts/rpc.py bdev_nvme_opal_revert -b nvme0 -p test || true
	sleep 1
	$rpc_py bdev_nvme_opal_init -b nvme0 -p test
	$rpc_py bdev_nvme_detach_controller nvme0

	killprocess $spdk_tgt_pid
}

function test_opal_cmds() {
	$rpc_py bdev_nvme_attach_controller -b "nvme0" -t "pcie" -a ${bdf}

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test
	$rpc_py bdev_opal_get_info -b nvme0n1r1 -p test

	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test

	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 3 -s 4096 -l 4096 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test

	$rpc_py bdev_opal_new_user -b nvme0n1r3 -p test -i 3 -u tester3
	$rpc_py bdev_opal_get_info -b nvme0n1r3 -p test
	$rpc_py bdev_opal_set_lock_state -b nvme0n1r3 -i 3 -p tester3 -l readonly
	$rpc_py bdev_opal_get_info -b nvme0n1r3 -p test
	$rpc_py bdev_opal_set_lock_state -b nvme0n1r1 -i 0 -p test -l rwlock

	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r3 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test

	$rpc_py bdev_nvme_detach_controller nvme0
}

function setup_test_environment() {
	$rpc_py bdev_nvme_attach_controller -b "nvme0" -t "pcie" -a ${bdf}

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 3 -s 4096 -l 4096 -p test

	$rpc_py bdev_opal_new_user -b nvme0n1r1 -p test -i 1 -u tester1
	$rpc_py bdev_opal_set_lock_state -b nvme0n1r1 -i 1 -p tester1 -l readwrite
	$rpc_py bdev_opal_new_user -b nvme0n1r3 -p test -i 3 -u tester3
	$rpc_py bdev_opal_set_lock_state -b nvme0n1r3 -i 3 -p tester3 -l readwrite

	$rpc_py bdev_opal_set_lock_state -b nvme0n1r2 -i 0 -p test -l readwrite
}

function clean_up() {
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r3 -p test
}

function revert() {
	$rpc_py bdev_nvme_opal_revert -b nvme0 -p test
}

function opal_spdk_tgt() {
	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!
	trap 'killprocess $spdk_tgt_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_tgt_pid
	test_opal_cmds
	killprocess $spdk_tgt_pid
}

function opal_bdevio() {
	$rootdir/test/bdev/bdevio/bdevio -w &
	bdevio_pid=$!
	trap 'killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevio_pid
	setup_test_environment
	$rootdir/test/bdev/bdevio/tests.py perform_tests
	clean_up
	$rpc_py bdev_nvme_detach_controller nvme0
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevio_pid
}

function opal_bdevperf() {
	$rootdir/test/bdev/bdevperf/bdevperf -z -q 8 -o 4096 -w verify -t 10 &
	bdevperf_pid=$!
	trap 'revert; killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $bdevperf_pid
	setup_test_environment
	$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
	clean_up
	revert
	$rpc_py bdev_nvme_detach_controller nvme0
	trap - SIGINT SIGTERM EXIT
	killprocess $bdevperf_pid
}

opal_revert_and_init

run_test "nvme_opal_spdk_tgt" opal_spdk_tgt
run_test "nvme_opal_bdevio" opal_bdevio
run_test "nvme_opal_bdevperf" opal_bdevperf
