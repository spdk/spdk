#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

NVME_DISK=""
NVME_DISK=""

bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
bdf2="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk1)"

function scsi_test_case1(){
    echo "Test Case 1 scsi"

    # unbind nvme disk
    unbind_bdev $bdf
    sleep 1

    # back configuration
    back_configuration $bdf
}

function scsi_test_case2() {
    echo "Test Case 2 scsi"

    # prepare configuration
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.1 Nvme0n1p1

    vms_start $used_vms
    get_disk $single_vm
    # unbind nvme disk and check retcode
    set +e
    unbind_bdev $bdf

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
    vms_reboot $used_vms

    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 2: After reboot disk." 1 $?
        if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
            echo "Error: NVMe disk is found"
        fi
    done

    # back configuration
    set -e
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.1
    back_configuration $bdf
}

function scsi_test_case3() {
    echo "Test Case 3 scsi"

    # prepare configuration
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1

    vm_number 0
    vms_start $single_vm

    # unbind nvme disk and check retcode
    get_disk $single_vm
    set +e
    unbind_bdev $bdf
    prepare_fio $single_vm

    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    # reboot vms and check fio
    vms_reboot $single_vm

    prepare_fio $single_vm
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1 $?
    if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    # back configuration
    set -e
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme1n1p0.1
    back_configuration $bdf
}

function scsi_test_case4() {
    echo "Test Case 4 blk"

    # prepare configuration
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p1.0

    vms_start $used_vms

    # unbind nvme disk and check retcode
    vm_list=( $used_vms )
    get_disk ${vm_list[1]}
    NVME_DISK2=$NVME_DISK
    get_disk ${vm_list[0]}
    set +e
    unbind_bdev $bdf

    fio_pid=""
    for vm_num in $used_vms; do
        if [[ $vm_num == $VM_NUM ]]; then
            prepare_fio_scsi $vm_num
           $run_fio &
           fio_pid=$!

        else
            prepare_fio_scsi $vm_num
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
    vms_reboot $used_vms

    for vm_num in $used_vms; do
        prepare_fio_scsi $vm_num
        $run_fio
        if [[ $vm_num == $VM_NUM ]]; then
           wait $!
           check_fio_retcode "BLK HOT REMOVE test case 4: After reboot disk." 0 $?
           if $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK>>/dev/null"); then
                echo "Error: NVMe disk is found"
            fi
        else
           check_fio_retcode "BLK HOT  REMOVE test case 4: After reboot disk." 1 $?
            if ! $(vm_ssh $vm_num "lsblk -d /dev/$NVME_DISK2>>/dev/null"); then
                echo "Error: NVMe disk is found"
            fi
        fi
    done

    # back configuration
    set -e
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.1
    $rpc_py remove_vhost_controller naa.Nvme1n1p0.1
    $rpc_py remove_vhost_controller naa.Nvme1n1p1.0
    back_configuration $bdf
}

config_base
run_vhost
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
if [[ $test_case == "test_case4" ||  $test_case == "all" ]]; then
	scsi_test_case4
fi
spdk_vhost_kill
