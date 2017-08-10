#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

VM_NUM=""
NVME_DISK=""

function get_disk() {
    vm_check_scsi_location $1
    disk_array=( $SCSI_DISK )
    NVME_DISK=${disk_array[0]}
}

function vm_number(){
    vms_array=( $used_vms )
    for vm in  $used_vms; do
        echo $vm
    done
    sleep 30
    VM_NUM=${vms_array[$1]}
    echo $VM_NUM
}

function prepare_fio() {
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_job
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_scsi_location $vm_num
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]"
            echo "filename=/dev/$disk"
            echo "time_based=20"
        done >> $tmp_job
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_4discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
        rm $tmp_job
    done
}

function create_scsi_controller() {
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller $1
}

function config_base(){
    cat $BASE_DIR/vhost.conf.base
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh
    echo "HotplugEnable Yes"
}

function gen_tc_1_blk_config() {
    config_base
}
function gen_tc_2_blk_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 1"
}

function gen_tc_3_blk_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 1"
    echo "  Split Nvme1n1 1"
}
function gen_tc_4_blk_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 2"
    echo "  Split Nvme1n1 2"
}

function unbind_bdev() {
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    (sleep 5; echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind) &
}

function vms_reboot(){
    vms_reboot_all
    vms_prepare
}

function vms_start(){
    vms_setup_and_run
    vms_prepare
    sleep 5
}
function check_fio_after_reboot(){
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After reboot vm." 1 $?
}
function scsi_test_case1(){
    echo "Test Case 1 blk"
    # prepare configuration
    gen_tc_1_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost

    # unbind nvme disk
    unbind_bdev
    sleep 10
    # back configuration
    rm $BASE_DIR/vhost.conf.in
    spdk_vhost_kill
    echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
    echo "=======finished1========">>log
}

function scsi_test_case2() {
    echo "Test Case 2 blk"

    # prepare configuration
    gen_tc_2_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost

    create_blk_controller naa.Nvme0n1p0.0
    vm_number 0
    vms_start
    # unbind nvme disk and check retcode
    get_disk
    set +xe
    unbind_bdev
    prepare_fio 0
    $run_fio
    check_fio_retcode "BLK HOT  REMOVE test case 2: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    # reboot vms and check fio
    vms_reboot

    prepare_fio 0
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    # back configuration
    back_configuration
    echo "=======finished2========">>log
}

function scsi_test_case3() {
    echo "Test Case 3 blk"
    gen_tc_3_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0
    create_blk_controller naa.Nvme1n1p0.1

    vm_number 0
    vms_start

    get_disk
    set +xe
    unbind_bdev
    prepare_fio 0
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    vms_reboot

    prepare_fio 0
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    back_configuration
    echo "=======finished3========">>log
}

function scsi_test_case4() {
    echo "Test Case 4 blk"
    gen_tc_4_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0
    create_blk_controller naa.Nvme0n1p1.1
    create_blk_controller naa.Nvme1n1p0.1
    create_blk_controller naa.Nvme1n1p1.0

    vm_number 0
    vms_start

    get_disk
    set +xe
    unbind_bdev
    fio_pid=""
    for vm_num in $used_vms; do
        if [[ $vm_num == $VM_NUM ]]; then
            prepare_fio $vm_num
           echo "run fio $vm_num"
           $run_fio &
           # pass fio
           fio_pid=$!

        else
            prepare_fio $vm_num
            echo "run fio $vm_num"
            $run_fio &
            (wait $!; retcode=$?; check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1 $retcode) &
        fi
    done
    # wait to finished fio in other disk
    wait $fio_pid
    tmp=$?
    echo $tmp
    sleep 30
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 0 $tmp
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        if [[ $vm_num == $VM_NUM ]]; then
           wait $!
           check_fio_retcode "BLK HOT REMOVE test case 2: After reboot disk." 0 $?
           if $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
                echo "Error: NVMe disk is found"
            fi
        else
           check_fio_retcode "BLK HOT  REMOVE test case 2: After reboot disk." 1 $?
            if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
                echo "Error: NVMe disk is found"
            fi
        fi
    done

    back_configuration
    echo "=======finished4========">>log
}

## Start  blk test
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

