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
        vm_check_scsi_location $vm_num
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]" >> $tmp_detach_job
            echo "filename=/dev/$disk" >> $tmp_detach_job
        done
        vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_2discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_2discs.job "
        rm $tmp_detach_job
    done
}

function scsi_hotremove_tc1() {
    echo "Scsi hotremove test case 1"
    disks=""
    get_disks "4" disks
    traddr=""
    get_traddr "Nvme0" traddr
    unbind_nvme "$traddr"
    sleep 1
    $rpc_py get_bdevs
    new_disks=""
    get_disks "4" new_disks
    bind_nvme "$traddr"
    sleep 1
    $rpc_py get_bdevs
    #check_disks "$disks" "$new_disks"

    #bind_nvme "$traddr"
    vm_kill "4"
    #get_disks "4" new_disks
}

function scsi_hotremove_tc2() {
    echo "Scsi hotremove test case 2"
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p8.4 0 HotInNvme0n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p9.4 0 HotInNvme0n1p1
    vms_setup_and_run_with_arg "4"
    vms_prepare "4"
    disks=""
    get_disks "4" disks
    traddr=""
    get_traddr "Nvme0" traddr
    prepare_fio_cmd_tc1 "4"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    set +xe
    wait $last_pid
    check_fio_retcode "Scsi hotremove test case 2: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 2: Iteration 2." 1 $?
    vm_kill "4"
    bind_nvme "$traddr"
    sleep 1
}

function scsi_hotremove_tc3() {
    echo "Scsi hotremove test case 3"
    $SPDK_BUILD_DIR/scripts/rpc.py get_bdevs
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p8.4 0 HotInNvme1n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p9.4 0 Malloc0
    vms_setup_and_run_with_arg "4"
    vms_prepare "4"
    disks=""
    get_disks "4" disks
    traddr=""
    get_traddr "Nvme0" traddr
    prepare_fio_cmd_tc1 "4"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    set +xe
    wait $last_pid
    check_fio_retcode "Scsi hotremove test case 3: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 3: Iteration 2." 1 $?
    vm_kill "4"
    bind_nvme "$traddr"
    sleep 1
}

function scsi_hotremove_tc4() {
    echo "Scsi hotremove test case 4"
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_scsi_dev naa.Nvme0n1p9.4 0
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p8.4 0 HotInNvme2n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p9.4 0 HotInNvme2n1p1
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p11.4 0 Malloc0
    vms_setup_and_run_with_arg "4"
    vms_prepare "4"
    disks=""
    get_disks "4" disks
    prepare_fio_cmd_tc1 "4"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    set +xe
    wait $last_pid
    check_fio_retcode "Scsi hotremove test case 4: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 4: Iteration 2." 1 $?
    vm_kill "4"
    bind_nvme "$traddr"
    sleep 1
}


scsi_hotremove_tc1
scsi_hotremove_tc2
scsi_hotremove_tc3
scsi_hotremove_tc4
#disk=""
#get_disks 1 disks
#echo $disks
#unbind_nvme 0000:08:00.0
#bind_nvme 0000:08:00.0
