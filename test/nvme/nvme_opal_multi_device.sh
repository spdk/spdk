#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py="$rootdir/scripts/rpc.py"
source "$rootdir/scripts/common.sh"
source "$rootdir/test/common/autotest_common.sh"

function opal_init() {
for num in 0 1 2 3
do
	bdf=$($rootdir/scripts/gen_nvme.sh --json | jq -r ".config[].params | select(.name==\"Nvme$num\").traddr")
	$rpc_py bdev_nvme_attach_controller -b "nvme$num" -t "pcie" -a $bdf
	
	# Ignore bdev_nvme_opal_init failure because sometimes revert TPer might fail and
	# in another run we don't want init to return errors to stop other tests.
	set +e
	$rpc_py bdev_nvme_opal_discovery -b nvme$num
	$rpc_py bdev_nvme_opal_init -b nvme$num -p test
	set -e
done
}

function test_opal_cmds() {
for num in 0 1 3	# only nvme0, nvme1, nvme3 is opal supported
do
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 2 -s 1024 -l 512 -p test
	$rpc_py bdev_opal_get_info -b nvme${num}n1r1 -p test
done

for num in 0 1 3
do
	$rpc_py bdev_opal_delete -b nvme${num}n1r1 -p test
	$rpc_py bdev_opal_delete -b nvme${num}n1r2 -p test
done

for num in 0 1 3
do
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 2 -s 1024 -l 512 -p test

	$rpc_py bdev_opal_delete -b nvme${num}n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme${num}n1r1 -p test
done

for num in 0 1 3
do
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 3 -s 4096 -l 4096 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 2 -s 1024 -l 512 -p test

	$rpc_py bdev_nvme_detach_controller nvme$num

	bdf=$($rootdir/scripts/gen_nvme.sh --json | jq -r ".config[].params | select(.name==\"Nvme$num\").traddr")
	$rpc_py bdev_nvme_attach_controller -b "nvme$num" -t "pcie" -a $bdf
	$rpc_py bdev_nvme_opal_discovery -b nvme$num
	$rpc_py bdev_opal_recovery -b nvme$num -n 1 -p test

	$rpc_py bdev_opal_new_user -b nvme${num}n1r3 -p test -i 3 -u tester3
	$rpc_py bdev_opal_get_info -b nvme${num}n1r3 -p test
	$rpc_py bdev_opal_set_lock_state -b nvme${num}n1r3 -i 3 -p tester3 -l readonly
	$rpc_py bdev_opal_get_info -b nvme${num}n1r3 -p test
	$rpc_py bdev_opal_set_lock_state -b nvme${num}n1r1 -i 0 -p test -l rwlock

	$rpc_py bdev_opal_delete -b nvme${num}n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme${num}n1r3 -p test
	$rpc_py bdev_opal_delete -b nvme${num}n1r1 -p test
done
}

function setup_test_environment() {
	opal_init
for num in 0 1 3
do
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 1 -s 0 -l 1024 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 2 -s 1024 -l 512 -p test
	$rpc_py bdev_opal_create -b nvme${num} -n 1 -i 3 -s 4096 -l 4096 -p test

	$rpc_py bdev_opal_new_user -b nvme${num}n1r1 -p test -i 1 -u tester1
	$rpc_py bdev_opal_set_lock_state -b nvme${num}n1r1 -i 1 -p tester1 -l readwrite
	$rpc_py bdev_opal_new_user -b nvme${num}n1r3 -p test -i 3 -u tester3
	$rpc_py bdev_opal_set_lock_state -b nvme${num}n1r3 -i 3 -p tester3 -l readwrite

	$rpc_py bdev_opal_set_lock_state -b nvme${num}n1r2 -i 0 -p test -l readwrite
done
}

function clean_up() {
	$rpc_py bdev_opal_delete -b nvme0n1r1 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r2 -p test
	$rpc_py bdev_opal_delete -b nvme0n1r3 -p test
}

function revert() {
	# Ignore revert failure
for num in 3 1 0
do	
	set +e
	$rpc_py bdev_nvme_opal_revert -b "nvme$num" -p test
	set -e
done
}

$rootdir/app/spdk_tgt/spdk_tgt &
spdk_tgt_pid=$!
trap 'killprocess $spdk_tgt_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_tgt_pid
opal_init
test_opal_cmds
killprocess $spdk_tgt_pid

$rootdir/test/bdev/bdevio/bdevio -w &
bdevio_pid=$!
trap 'revert; killprocess $bdevio_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevio_pid
setup_test_environment
$rootdir/test/bdev/bdevio/tests.py perform_tests
clean_up
revert
sleep 500
trap - SIGINT SIGTERM EXIT
killprocess $bdevio_pid

