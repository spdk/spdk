#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

function run_bdevperf_test {
	trap - ERR
	set +xe
	$rootdir/test/bdev/bdevperf/bdevperf.py -s "$1" perform_tests
	ret_value=$?
	if [ $ret_value -ne 0 ]; then
		echo "Perform test ended with error"
		return 1
	fi
}

trap 'killprocess $bdevperf_pid1; killprocess $bdevperf_pid2; vhost_kill 0; exit 1' ERR
vhost_run 0
traddr=$($rpc_py bdev_get_bdevs -b "Nvme0n1" | jq -r '.[].driver_specific.nvme.pci_address')
$rpc_py bdev_split_create Nvme0n1 2
$rpc_py vhost_create_scsi_controller scsi_ctrl
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 Nvme0n1p0
$rpc_py vhost_create_blk_controller blk_ctrl Nvme0n1p1

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev_scsi.conf -g -q 128 -o 4096 -w verify -z -t 15 -r /var/tmp/spdk1.sock &
bdevperf_pid1=$!
waitforlisten $bdevperf_pid1 /var/tmp/spdk1.sock
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev_blk.conf -g -q 128 -o 4096 -w verify -z -t 15 -r /var/tmp/spdk2.sock &
bdevperf_pid2=$!
waitforlisten $bdevperf_pid2 /var/tmp/spdk2.sock
run_bdevperf_test /var/tmp/spdk1.sock &
perftest_pid1=$!
run_bdevperf_test /var/tmp/spdk2.sock &
perftest_pid2=$!
sleep 5
# Do hotremove
$rpc_py bdev_nvme_detach_controller "Nvme0"
sleep 10
if wait $perftest_pid1; then
	error "Hot-remove for scsi exited with success when error was expected"
fi
if wait $perftest_pid2; then
        error "Hot-remove for blk exited with success when error was expected"
fi
if ! killprocess $bdevperf_pid2; then
        error "Bdevperf ended with fail but shouldn't"
fi
if ! kill -0 $bdevperf_pid1 2>/dev/null; then
        error "Bdevperf with scsi controller exited but shouldn't"
fi

# Do hotattach
$rpc_py vhost_delete_controller blk_ctrl
$rpc_py bdev_nvme_attach_controller -b "Nvme0" -t PCIe -a "$traddr"
$rpc_py bdev_split_create Nvme0n1 2
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 Nvme0n1p0
sleep 1
run_bdevperf_test /var/tmp/spdk1.sock &
perftest_pid1=$!
if ! wait $perftest_pid1; then
        error "Hot-attach failed"
fi
killprocess $bdevperf_pid1

$rpc_py vhost_scsi_controller_remove_target scsi_ctrl 0
$rpc_py vhost_delete_controller scsi_ctrl
vhost_kill 0
