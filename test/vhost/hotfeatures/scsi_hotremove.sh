#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

VM_NUM=""
NVME_DISK=""

function scsi_test_case1(){
    echo "Test Case 1 scsi"
    # prepare configuration
    config_base 0 0
    run_vhost

    # unbind nvme disk
    unbind_bdev
    sleep 1

    # back configuration
    back_configuration
}

function scsi_test_case2() {
    echo "Test Case 2 scsi"

    # prepare configuration
    config_base 1 2
    run_vhost
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1

    vms_start

    # unbind nvme disk and check retcode
    set +xe
    unbind_bdev

    for vm_num in $used_vms; do
        (prepare_fio $vm_num;
        $run_fio;
        wait $!
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1 $?) &
        if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
            echo "Error: NVMe disk is found"
        fi
    done

    # reboot vms and check fio
    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 2: After reboot disk." 1 $?
        if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
            echo "Error: NVMe disk is found"
        fi
    done

    # back configuration
    set -xe
    back_configuration
}

function scsi_test_case3() {
    echo "Test Case 3 scsi"

    # prepare configuration
    config_base 2 1
    run_vhost
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1

    vm_number 0
    vms_start

    # unbind nvme disk and check retcode
    get_disk
    set +xe
    unbind_bdev
    prepare_fio_scsi 0

    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    # reboot vms and check fio
    vms_reboot

    prepare_fio_scsi 0
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    # back configuration
    set -xe
    back_configuration
}

function scsi_test_case4() {
    echo "Test Case 4 blk"

    # prepare configuratio
    config_base 2 2
    run_vhost
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p1.0

    vm_number 0
    vms_start

    # unbind nvme disk and check retcode
    get_disk
    set +xe
    unbind_bdev

    fio_pid=""
    for vm_num in $used_vms; do
        if [[ $vm_num == $VM_NUM ]]; then
            prepare_fio_scsi $vm_num
           echo "run fio $vm_num"
           $run_fio &
           # pass fio
           fio_pid=$!

        else
            prepare_fio_scsi $vm_num
            echo "run fio $vm_num"
            $run_fio &
            (wait $!; retcode=$?; check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1 $retcode) &
        fi
    done

    # wait to finished fio in other disk
    wait $fio_pid
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 0 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    # reboot vms and check fio
    vms_reboot

    for vm_num in $used_vms; do
        prepare_fio_scsi $vm_num
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

    # back configuration
    set -xe
    back_configuration
}

## Start  blk test
if [[ $test_case == "test_case1" ||  $test_case == "all" ]]; then
    scsi_test_case1
fi
if [[ $test_case == "test_case2" ||  $test_case == "all" ]]; then
    scsi_test_case2
fi
if [[ $test_case == "test_case3" ||  $test_case == "all" ]]; then
    scsi_test_case3
fi
if [[ $test_case == "test_case4" ||  $test_case == "all"]]; then
    scsi_test_case4
fi

