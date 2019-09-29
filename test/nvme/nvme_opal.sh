#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"

function opal_init() {
	bdf1=$($rootdir/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
	$rpc_py bdev_nvme_attach_controller -b "nvme0" -t "pcie" -a $bdf1

	# Ignore bdev_nvme_opal_init failure because sometimes revert TPer might fail and
	# in another run we don't want init to return errors to stop other tests.
	set +e
	$rpc_py bdev_nvme_opal_init -b nvme0 -p test
	set -e
}

function setup_test_environment() {
	bdf1=$($rootdir/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
	$rpc_py bdev_nvme_attach_controller -b "nvme0" -t "pcie" -a $bdf1
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test
	$rpc_py bdev_opal_get_info -b nvme0n1r1 -p test
	$rpc_py bdev_opal_get_info -b nvme0n1r2 -p test

	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test

	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test

	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 3 -s 4096 -l 4096 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme0 -n 1 -i 2 -s 1024 -l 512 -p test
}

function clean_up() {
	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r3 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test

	$rpc_py bdev_nvme_opal_revert -b nvme0 -p test
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_tgt_pid=$!
trap 'killprocess $spdk_tgt_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_tgt_pid
opal_init
killprocess $spdk_tgt_pid

$rootdir/test/bdev/bdevio/bdevio -w &
bdevio_pid=$!
trap 'killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevio_pid
setup_test_environment
$rootdir/test/bdev/bdevio/tests.py perform_tests
trap - SIGINT SIGTERM EXIT
killprocess $bdevio_pid

$rootdir/test/bdev/bdevperf/bdevperf -z -q 8 -o 4096 -w verify -t 10 &
bdevperf_pid=$!
trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid
setup_test_environment
$rootdir/test/bdev/bdevperf/bdevperf.py perform_tests
clean_up
trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid
