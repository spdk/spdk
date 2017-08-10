#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

VM_NUM=""
bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
bdf2="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk1)"

function blk_test_case1(){
    echo "Test Case 1 blk"

    # unbind nvme disk
    unbind_bdev $bdf
    sleep 1

    # back configuration
    back_configuration $bdf
}

function blk_test_case2() {
    echo "Test Case 2 blk"

    # prepare configuration
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    vm_number 0
    vms_start $single_vm

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev $bdf
    prepare_fio $single_vm
    $run_fio
    check_fio_retcode "BLK HOT  REMOVE test case 2: After unbind disk." 1 $?

    # reboot vms and check fio
    vms_reboot $single_vm

    prepare_fio $single_vm
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?

    # back configuration
    set -xe
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    back_configuration $bdf
}

function blk_test_case3() {
    echo "Test Case 3 blk"

    # prepare configuration
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p0.1 Nvme1n1p0

    vm_number 0
    vms_start $single_vm

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev $bdf
    prepare_fio $single_vm
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?

    vms_reboot $single_vm

    prepare_fio $single_vm
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?

    # back configuration
    set -xe
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme1n1p0.1
    back_configuration $bdf
}

function blk_test_case4() {
    echo "Test Case 4 blk"

    # prepare configuration
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p0.1 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p1.0 Nvme1n1p1

    vms_start $used_vms

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev $bdf2

    fio_pid=""
    for vm_num in $used_vms; do
        if [[ $vm_num == $single_vm ]]; then
           prepare_fio $vm_num
           $run_fio &
           fio_pid=$!
        else
            prepare_fio $vm_num
            $run_fio &
            (wait $!; retcode=$?; check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1 $retcode) &
        fi
    done

    wait $fio_pid
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 0 $?

    vms_reboot $used_vms

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        if [[ $vm_num == $single_vm ]]; then
           wait $!
           check_fio_retcode "BLK HOT REMOVE test case 4: After reboot disk." 0 $?
        else
           check_fio_retcode "BLK HOT  REMOVE test case 4: After reboot disk." 1 $?
        fi
    done

    # back configuration
    set -xe
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.1
    $rpc_py remove_vhost_controller naa.Nvme1n1p0.1
    $rpc_py remove_vhost_controller naa.Nvme1n1p1.0
    back_configuration $bdf
}

function blk_test_case5() {
    echo "Test Case 5 blk"

    # prepare configuration
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1

    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev $bdf

    for vm_num in $used_vms; do
        (prepare_fio $vm_num;
         $run_fio;
         wait $!
         check_fio_retcode "BLK HOT REMOVE test case 5: After unbind disk."
         1 $?) &
    done

    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 5: After reboot disk." 1 $?
    done

    # back configuration
    set -xe
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.1
    back_configuration $bdf
}

config_base
run_vhost
## Start  blk test
if [[ $test_case == "test_case1" ||  $test_case == "all" ]]; then
    blk_test_case1
fi
if [[ $test_case == "test_case2" ||  $test_case == "all" ]]; then
    blk_test_case2
fi
if [[ $test_case == "test_case3" ||  $test_case == "all" ]]; then
    blk_test_case3
fi
if [[ $test_case == "test_case4" ||  $test_case == "all" ]]; then
	blk_test_case4
fi
if [[ $test_case == "test_case5" ||  $test_case == "all" ]]; then
    blk_test_case5
fi
spdk_vhost_kill
