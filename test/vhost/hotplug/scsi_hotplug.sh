#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

# Add split section into vhost config
function gen_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 16
  Split Nvme1n1 20
  Split HotInNvme0n1 2
  Split HotInNvme1n1 2
  Split HotInNvme2n1 2
END_OF_CONFIG
}

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
    vms_setup
    vm_run_with_arg $used_vms
    vms_prepare $used_vms
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
gen_config
if [ $current_driver == "vfio" ] && [ $scsi_hot_remove_test == 1 ]; then
    switch_to_uio
fi
run_vhost
sleep 10
rm $BASE_DIR/vhost.conf.in
if [ $scsi_hot_remove_test == 0 ]; then
    pre_hot_attach_detach_test_case
    $BASE_DIR/scsi_hotattach.sh --fio-bin=$fio_bin &
    first_script=$!
    $BASE_DIR/scsi_hotdetach.sh --fio-bin=$fio_bin &
    second_script=$!
    wait $first_script
    wait $second_script
    vm_shutdown_all
    post_hot_attach_detach_test_case
fi
vm_arg=""
for vm in "${vms[@]}"; do
    vm_arg+="--vm=$vm "
done
if [ $scsi_hot_remove_test == 1 ];then
    $BASE_DIR/scsi_hotremove.sh --fio-bin=$fio_bin $vm_arg --test-type=spdk_vhost_scsi
fi
post_test_case
