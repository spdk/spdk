#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

function run_spdk_fio() {
	fio_bdev --ioengine=spdk_bdev \
		"$rootdir/test/vhost/common/fio_jobs/default_initiator.job" --runtime=10 --rw=randrw \
		--spdk_mem=1024 --spdk_single_seg=1 --spdk_json_conf=$testdir/bdev.json "$@"
}

vhosttestinit "--no_vm"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR SIGTERM SIGABRT

vhost_run 0

$rpc_py bdev_malloc_create -b Malloc 124 4096
$rpc_py vhost_create_blk_controller Malloc.0 Malloc

run_spdk_fio --size=50% --offset=0 --filename=VirtioBlk0 &
run_fio_pid=$!
sleep 1
run_spdk_fio --size=50% --offset=50% --filename=VirtioBlk0
wait $run_fio_pid
vhost_kill 0

vhosttestfini
