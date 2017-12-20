#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function prepare_fio_cmd_tc1() {
    print_test_fio_header

    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_detach_job
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_blk_location $vm_num
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]" >> $tmp_detach_job
            echo "filename=/dev/$disk" >> $tmp_detach_job
        done
        vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_2discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_2discs.job "
        rm $tmp_detach_job
    done
}

function blk_hotremove_tc1() {
    echo "Blk hotremove test case 1"
    traddr=""
    get_traddr "Nvme0" traddr
    unbind_nvme "$traddr"
    sleep 1
    $rpc_py get_bdevs
    bind_nvme "$traddr"
    sleep 1
    $rpc_py get_bdevs
}

function blk_hotremove_tc2() {
    echo "Blk hotremove test case 2"
    $rpc_py get_bdevs
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 Malloc1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Malloc2
    vms_setup
    vm_run_with_arg "0 1"
    vms_prepare "0"
    vm_ssh 0 "lsblk"

    traddr=""
    get_traddr "Nvme0" traddr
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid1=$!
    sleep 3
    unbind_nvme "$traddr"
    wait $last_pid1 || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 2: Iteration 1." 1 $retcode

    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 2: Iteration 2." 1 $retcode
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc3() {
    echo "Blk hotremove test case 3"
    $rpc_py get_bdevs
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 HotInNvme1n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Malloc1
    vm_run_with_arg "0 1"
    vms_prepare "0 1"

    traddr=""
    get_traddr "Nvme0" traddr
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    wait $last_pid || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 3: Iteration 1." 1 $retcode

    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 3: Iteration 2." 1 $retcode
    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc4() {
    echo "Blk hotremove test case 4"
    $rpc_py get_bdevs
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme2n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 HotInNvme2n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Malloc1
    vm_run_with_arg "0 1"
    vms_prepare "0 1"

    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid_vm0=$!

    prepare_fio_cmd_tc1 "1"
    $run_fio &
    last_pid_vm1=$!

    sleep 3
    unbind_nvme "$traddr"
    wait $last_pid_vm0 || retcode_vm0=$? || true
    wait $last_pid_vm1 || retcode_vm1=$? || true
    check_fio_retcode "Blk hotremove test case 4: Iteration 1." 1 $retcode_vm0
    check_fio_retcode "Blk hotremove test case 4: Iteration 2." 1 $retcode_vm1

    reboot_all_and_prepare "0 1"
    $run_fio|| retcode=$? || true
    check_fio_retcode "Blk hotremove test case 4: Iteration 3." 1 $retcode

    vm_shutdown_all
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc5() {
    echo "Blk hotremove test case 5"
    $rpc_py get_bdevs
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme3n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 Malloc1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Malloc2
    vm_run_with_arg "0 1"
    vms_prepare "0 1"
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    wait $last_pid || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 5: Iteration 1." 1 $retcode
    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 5: Iteration 2." 1 $retcode
    vm_shutdown_all
    bind_nvme "$traddr"
    sleep 1
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
}

blk_hotremove_tc1
blk_hotremove_tc2
blk_hotremove_tc3
blk_hotremove_tc4
blk_hotremove_tc5

