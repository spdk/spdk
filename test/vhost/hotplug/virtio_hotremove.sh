#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

function run_bdevperf_test {
	if $rootdir/test/bdev/bdevperf/bdevperf.py perform_tests | grep "error"; then
		echo "Perform test ended with error"
		return 1
	fi
}

vhost_run 0
$rpc_py bdev_nvme_set_hotplug -e
$rpc_py bdev_split_create Nvme0n1 2
$rpc_py vhost_create_scsi_controller scsi_ctrl
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 Nvme0n1p0
$rpc_py vhost_create_blk_controller blk_ctrl Nvme0n1p1

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -g -q 128 -o 4096 -w verify -z -t 10 &
bdevperf_pid=$!
waitforlisten $bdevperf_pid
trap 'vhost_kill 0; killprocess $bdevperf_pid; exit 1' ERR
bdevperf_retcode=0
run_bdevperf_test &
perftest_pid=$!
sleep 5
# Do hotremove
$rpc_py bdev_nvme_detach_controller "Nvme0"
sleep 5
retcode=0
wait $perftest_pid || retcode=1
if [[ $retcode -eq 0 ]]; then
	error "Perform test exited with no error"
	exit 1
fi
retcode=0
killprocess $bdevperf_pid || retcode=1
if [[ $retcode -eq 0 ]]; then
	error "Bdevperf ended with success"
	exit 1
fi

trap 'vhost_kill 0; exit 1' ERR
get_traddr "Nvme0"
$rpc_py vhost_delete_controller blk_ctrl
$rpc_py bdev_nvme_attach_controller -b "HotInNvme1" -t PCIe -a "$traddr"
$rpc_py bdev_split_create HotInNvme1n1 2
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 HotInNvme1n1p0
$rpc_py vhost_create_blk_controller blk_ctrl HotInNvme1n1p1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -g -q 128 -o 4096 -w verify -t 5

$rpc_py vhost_scsi_controller_remove_target scsi_ctrl 0
$rpc_py vhost_delete_controller scsi_ctrl
$rpc_py vhost_delete_controller blk_ctrl
vhost_kill 0
