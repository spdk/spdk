#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

NAME_DISK="NVMe0"

. $COMMON_DIR/common.sh
. $COMMON_DIR/common_hotfeatures.sh

function scsi_test_case1(){
    echo "Test Case 1 SCSI"
    check_qemu
    check_spdk
    gen_scsi_config_and_run_vhost
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf "NVMe0")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    sleep 5
    spdk_vhost_kill
    sudo echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
}

function scsi_test_case2() {
    echo "Test Case 2 SCSI"
    check_qemu
    check_spdk
    gen_scsi_config_and_run_vhost
    setup_and_run_vm
    prepare_vm
    check_lsblk_in_vm
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    check_lsblk_in_vm
    reboot_all_vm
    check_lsblk_in_vm
    run
    vm_kill_all
    spdk_vhost_kill
    sudo echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
}
