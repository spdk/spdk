#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function prepare_fio() {
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_job
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_blk_location $vm_num
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

function create_blk_controller() {
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller $1 $2
}

function config_base(){
    cat $BASE_DIR/vhost.conf.base
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh
    echo "HotplugEnable Yes"
}

function gen_tc_1_blk_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 1"
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

function gen_tc_5_blk_config() {
    config_base
    echo "[Split]"
    echo "  Split Nvme0n1 2"
}

function unbind_bdev() {
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    (sleep 5; echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind) &
    set +xe
}

function vms_reboot(){
    set -xe
    vms_reboot_all
    vms_prepare
    set +xe
}

function vms_start(){
    vms_setup_and_run
    vms_prepare
    sleep 5
}
function check_fio_after_reboot(){
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After reboot vm." 1 $?
    set -xe
}
function blk_test_case1(){
    echo "Test Case 1 blk"
    # prepare configuration
    gen_tc_1_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0

    # unbind nvme disk
    unbind_bdev

    # back configuration
    rm $BASE_DIR/vhost.conf.in
    spdk_vhost_kill
    echo $bdf > /sys/bus/pci/drivers/uio_pci_generic/bind
}

function blk_test_case2() {
    echo "Test Case 2 blk"

    # prepare configuration
    gen_tc_2_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0

    vms_start
    prepare_fio $used_vms

    # unbind nvme disk and check retcode
    unbind_bdev
    set +xe
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?

    # reboot vms and check fio
    vms_reboot

    check_fio_after_reboot

    # back configuration
    back_configuration
}

function blk_test_case3() {
    echo "Test Case 3 blk"
    gen_tc_3_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme1n1p0.1 Nvme1n1p1

    vms_start
    prepare_fio $used_vms

    unbind_bdev
    set +xe
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?

    vms_reboot

    check_fio_after_reboot

    back_configuration
}

function blk_test_case4() {
    echo "Test Case 4 blk"
    gen_tc_4_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme0n1p0.1 Nvme0n1p1
    create_blk_controller naa.Nvme1n1p0.1 Nvme1n1p1
    create_blk_controller naa.Nvme1n1p1.0 Nvme1n1p1

    vms_array=( $used_vms )
    vm_first=${vms_array[0]}

    vms_start

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        if [[ $vm_num == $vm_first ]]; then
           fio_pid=$!
        fi
        sleep 5
    done

    unbind_bdev
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1 $?
    wait $fio_pid
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 0 $?

    vms_reboot

    for vm_num in $used_vms; do
        $run_fio
        if [[ $vm_num == $first_vm ]]; then
           fio_pid=$!
           wait $fio_pid
        else
           check_fio_retcode "BLK HOT REMOVE test case 4: After reboot vm." $vm_num $?
        fi
    done

    back_configuration
}

function blk_test_case5() {
    echo "Test Case 5 blk"
    gen_tc_5_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme0n1p0.1 Nvme0n1p1

    vms_start

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        sleep 5
    done

    unbind_bdev

    for vm_num in $used_vms; do
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $!
    done

    vms_reboot

    for vm_num in $used_vms; do
        check_fio_after_reboot
    done

    back_configuration
}

## Start  blk test
if [[ $test_case == "test_case1" ]]; then
    blk_test_case1
fi
if [[ $test_case == "test_case2" ]]; then
    blk_test_case2
fi
if [[ $test_case == "test_case3" ]]; then
    blk_test_case3
fi
if [[ $test_case == "test_case4" ]]; then
    blk_test_case4
fi
if [[ $test_case == "test_case5" ]]; then
    blk_test_case5
fi
