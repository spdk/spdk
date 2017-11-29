#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"
current_driver="vfio"
if $BASE_DIR/../../../scripts/setup.sh status | grep uio; then
    current_driver="uio"
fi
# Add split section into vhost config
function gen_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 20
  Split Nvme1n1 20
  Split HotInNvme0n1 2
  Split HotInNvme1n1 2
  Split HotInNvme2n1 2
  Split HotInNvme3n1 2
  Split HotInNvme4n1 2
  Split HotInNvme5n1 2
  Split HotInNvme6n1 2
  Split HotInNvme7n1 2
END_OF_CONFIG
}

# Run spdk by calling run_vhost from hotplug/common.sh.
# Run_vhost uses run_vhost.sh (test/vhost/common) script.
# This script calls spdk_vhost_run (common/common.sh) to run vhost.
# Then prepare vhost with rpc calls and setup and run 4 VMs.
function pre_test_case() {
    used_vms=""
    run_vhost
    rm $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p1.0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p2.1
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p3.1
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p4.2
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p5.2
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p6.3
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p7.3
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p8.4
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p9.4
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p10.4
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p11.4
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p4.2 0 Nvme0n1p8
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p4.2 1 Nvme0n1p9
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p5.2 0 Nvme0n1p10
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p5.2 1 Nvme0n1p11
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p6.3 0 Nvme0n1p12
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p6.3 1 Nvme0n1p13
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p7.3 0 Nvme0n1p14
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p7.3 1 Nvme0n1p15
    $SPDK_BUILD_DIR/scripts/rpc.py construct_malloc_bdev -b Malloc0 128 4096
    $SPDK_BUILD_DIR/scripts/rpc.py construct_malloc_bdev -b Malloc1 128 4096
    $SPDK_BUILD_DIR/scripts/rpc.py construct_malloc_bdev -b Malloc2 128 4096
    vms_setup_and_run_with_arg "0 1"
    vms_prepare "0 1"
}

function cleanup_after_hot_attach_detach() {
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p4.2 0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p4.2 1
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p5.2 0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p5.2 1
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p6.3 0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p6.3 1
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p7.3 0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p7.3 1
}

function cleanup_after_scsi_hotremove() {
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p0.0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p1.0
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p2.1
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p3.1
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p4.2
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p5.2
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p6.3
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p7.3
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p8.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p9.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p10.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p11.4
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
gen_config
pre_test_case
#$BASE_DIR/scsi_hotattach.sh --fio-bin=$fio_bin &
#first_script=$!
#$BASE_DIR/scsi_hotdetach.sh --fio-bin=$fio_bin &
#second_script=$!
#wait $first_script
#wait $second_script
vm_shutdown_all
cleanup_after_hot_attach_detach
$BASE_DIR/scsi_hotremove.sh --fio-bin=$fio_bin --allow-vfio-to-uio
cleanup_after_scsi_hotremove
vm_arg=""
for vm in "${vms[@]}"; do
    vm_arg+="--vm=$vm "
done
$BASE_DIR/blk_hotremove.sh --fio-bin=$fio_bin $vm_arg --test-type=spdk_vhost_blk --allow-vfio-to-uio
post_test_case
