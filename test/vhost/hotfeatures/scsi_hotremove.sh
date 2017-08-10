#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function get_bdev_disk() {
    vm_check_scsi_location $1
    disk_array=( $SCSI_DISK )
    $2=${disk_array[0]}
}
function prepare_fio() {
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_job
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_scsi_location $vm_num
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]" >> $tmp_job
            echo "filename=/dev/$disk" >> $tmp_job
        done
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_4discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
        rm $tmp_job
    done
}
function create_scsi_controller() {
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller $1 $2
}

function config_base(){
    cat $BASE_DIR/vhost.conf.base
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh
    echo "HotplugEnable Yes"
}

function gen_tc_1_scsi_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 1"
}

function gen_tc_2_scsi_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 2"

function gen_tc_3_scsi_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 1"
    echo "  Split Nvme1n1 1"
}

function unbind_bdev() {
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    (sleep 5; echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind)
}
function check_bdevs() {
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

}
function scsi_test_case1(){
    echo "Test Case 1 SCSI HOT REMOVE"
    gen_tc_1_scsi_config > $BASE_DIR/vhost.conf.in
    run_vhost

    unbind_bdev &

    rm $BASE_DIR/vhost.conf.in
    spdk_vhost_kill
    echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
}


function scsi_test_case2() {
    echo "Test Case 2 SCSI HOTREMOVE"
    first_disk=""
    gen_tc_2_scsi_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_scsi_controller 0 Nvme0n1p0
    create_scsi_controller 0 Nvme0n1p1

    vms_setup_and_run
    vms_prepare
    sleep 5

    get_bdev_disk $used_vms first_disk
    prepare_fio $used_vms
    sleep 5

    unbind_bdev &
    $run_fio
    set +xe
    check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." 1 $?

    check_bdevs
    sleep 5

    vms_reboot_all
    vms_prepare

    check_bdevs
    prepare_fio $vm_num
    check_fio_after_reboot

    back_configuration
}

function scsi_test_case3() {
    echo "Test Case 3 SCSI HOTREMOVE"
    first_disk=""
    gen_tc_3_scsi_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_scsi_controller 0 Nvme0n1p0
    create_scsi_controller 0 Nvme1n1p0
    vms_setup_and_run
    prepare_vms
    sleep 5
    get_bdev_disk "0" first_disk

    prepare_fio $used_vms
    unbind_bdev &
    set  +xe
    $run_fio
    check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." 1 $?
    set -xe

    check_bdevs
    vms_reboot_all
    vms_prepare
    check_bdevs

    prepare_fio $vm_num
    check_fio_after_reboot
    back_configuration
}

function scsi_test_case4() {
    echo "Test Case 4 SCSI HOTREMOVE"
    first_disk=""
    second_disk=""
    gen_tc_3_scsi_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_scsi_controller 0 Nvme0n1p0
    create_scsi_controller 0 Nvme1n1p0

    vms_setup_and_run
    vms_prepare
    sleep 5

    vms_array=( $used_vms )
    vm_first=${vms_array[0]}
    vm_second=${vms_array[0]}
    get_bdev_disk $vm_first first_disk
    get_bdev_disk $vm_second second_disk

    prepare_fio 0
    $run_fio &
    prepare_fio 1
    $run_fio &
    fio_pid=$!
    sleep 5

    unbind_bdev
    set +xe
    check_fio_retcode "SCSI HOT REMOVE test case 4: After unbind disk." 1 $?
    wait $fio_pid
    set -xe
    check_fio_retcode "SCSI HOT REMOVE test case 4: After unbind disk." 0 $?
    check_bdevs

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        set +xe
        check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." $vm_num $?
        set -xe
        sleep 5
    done

    reboot_all_vms
    vms_prepare

    check_bdevs
    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        set +xe
        check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." $vm_num $?
        set -xe
        sleep 5
    done
    back_configuration
}

## Start  scsi test
if [[ $test_case == "test_case1" ]]; then
    scsi_test_case1
fi
if [[ $test_case == "test_case2" ]]; then
    scsi_test_case2
fi
if [[ $test_case == "test_case3" ]]; then
    scsi_test_case3
fi
if [[ $test_case == "test_case4" ]]; then
    scsi_test_case4
fi
