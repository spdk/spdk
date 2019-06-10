#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

if [[ $scsi_hot_remove_test == 1 ]] && [[ $blk_hot_remove_test == 1 ]]; then
    notice "Vhost-scsi and vhost-blk hotremove tests cannot be run together"
fi

# Run spdk by calling run_vhost from hotplug/common.sh.
# Then prepare vhost with rpc calls and setup and run 4 VMs.
function pre_hot_attach_detach_test_case() {
    used_vms=""
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p2.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p3.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p4.2
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p5.2
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p6.3
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p7.3
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p4.2 0 Nvme0n1p8
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p4.2 1 Nvme0n1p9
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p5.2 0 Nvme0n1p10
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p5.2 1 Nvme0n1p11
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p6.3 0 Nvme0n1p12
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p6.3 1 Nvme0n1p13
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p7.3 0 Nvme0n1p14
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p7.3 1 Nvme0n1p15
    vms_setup_and_run "0 1 2 3"
    vms_prepare "0 1 2 3"
}

function clear_vhost_config() {
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p4.2 0
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p4.2 1
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p5.2 0
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p5.2 1
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p6.3 0
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p6.3 1
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p7.3 0
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p7.3 1
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p4.2
    $rpc_py remove_vhost_controller naa.Nvme0n1p5.2
    $rpc_py remove_vhost_controller naa.Nvme0n1p6.3
    $rpc_py remove_vhost_controller naa.Nvme0n1p7.3
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
# Hotremove/hotattach/hotdetach test cases prerequisites
# Run vhost with 2 NVMe disks.

notice "==============="
notice ""
notice "running SPDK"
notice ""
vhost_run
$rpc_py set_bdev_nvme_hotplug -e
$rpc_py construct_split_vbdev Nvme0n1 16
$rpc_py construct_malloc_bdev 128 512 -b Malloc
$rpc_py construct_split_vbdev Malloc 4
$rpc_py construct_split_vbdev HotInNvme0n1 2
$rpc_py construct_split_vbdev HotInNvme1n1 2
$rpc_py construct_split_vbdev HotInNvme2n1 2
$rpc_py construct_split_vbdev HotInNvme3n1 2
$rpc_py get_bdevs

if [[ $scsi_hot_remove_test == 0 ]] && [[ $blk_hot_remove_test == 0 ]]; then
    pre_hot_attach_detach_test_case
    $testdir/scsi_hotattach.sh --fio-bin=$fio_bin &
    first_script=$!
    $testdir/scsi_hotdetach.sh --fio-bin=$fio_bin &
    second_script=$!
    wait $first_script
    wait $second_script
    vm_shutdown_all
    clear_vhost_config
fi
if [[ $scsi_hot_remove_test == 1 ]]; then
    source $testdir/scsi_hotremove.sh
fi
if [[ $blk_hot_remove_test == 1 ]]; then
    source $testdir/blk_hotremove.sh
fi
post_test_case
