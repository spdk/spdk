#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

VM_NUM=""

function create_blk_controller(){
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller $1 $2
}

# extension of the configuration file
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

function blk_test_case1(){
    echo "Test Case 1 blk"

    # prepare configuration
    config_base > $BASE_DIR/vhost.conf.in
    run_vhost

    # unbind nvme disk
    unbind_bdev
    sleep 10

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
    vm_number 0
    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev
    prepare_fio $VM_NUM
    $run_fio
    check_fio_retcode "BLK HOT  REMOVE test case 2: After unbind disk." 1 $?

    # reboot vms and check fio
    vms_reboot

    prepare_fio $VM_NUM
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?

    # back configuration
    set -xe
    back_configuration
}

function blk_test_case3() {
    echo "Test Case 3 blk"

    # prepare configuration
    gen_tc_3_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme1n1p0.1 Nvme1n1p0

    vm_number 0
    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev
    prepare_fio  $VM_NUM
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?

    vms_reboot

    prepare_fio  $VM_NUM
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?

    # back configuration
    set -xe
    back_configuration
}

function blk_test_case4() {
    echo "Test Case 4 blk"

    # prepare configuration
    gen_tc_4_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1
    create_blk_controller naa.Nvme1n1p0.1 Nvme1n1p0
    create_blk_controller naa.Nvme1n1p1.0 Nvme1n1p1

    vm_number 0
    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev

    fio_pid=""
    for vm_num in $used_vms; do
        if [[ $vm_num == $VM_NUM ]]; then
            prepare_fio $vm_num
           echo "run fio $vm_num"
           $run_fio &
           fio_pid=$!
        else
            prepare_fio $vm_num
            echo "run fio $vm_num"
            $run_fio &
            (wait $!; retcode=$?; check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1 $retcode) &
        fi
    done

    wait $fio_pid
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 0 $?

    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        if [[ $vm_num == $VM_NUM ]]; then
           wait $!
           check_fio_retcode "BLK HOT REMOVE test case 2: After reboot disk." 0 $?
        else
           check_fio_retcode "BLK HOT  REMOVE test case 2: After reboot disk." 1 $?
        fi
    done

    # back configuration
    set -xe
    back_configuration
}

function blk_test_case5() {
    echo "Test Case 5 blk"

    # prepare configuration
    gen_tc_5_blk_config > $BASE_DIR/vhost.conf.in
    run_vhost
    create_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    create_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1

    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev

    for vm_num in $used_vms; do
        (prepare_fio $vm_num;
         $run_fio;
         wait $!
         check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?) &
    done

    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 2: After reboot disk." 1 $?
    done

    # back configuration
    set -xe
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
