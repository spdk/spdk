set -xe

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
            echo "size=100%" >> $tmp_detach_job
        done
        vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_2discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_2discs.job "
        rm $tmp_detach_job
    done
}

function scsi_hotremove_tc1() {
    echo "Scsi hotremove test case 1"
    traddr=""
    get_traddr "Nvme0"
    delete_nvme "Nvme0n1"
    sleep 1
    add_nvme "HotInNvme0" "$traddr"
}

function scsi_hotremove_tc2() {
    echo "Scsi hotremove test case 2"
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 HotInNvme0n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme1n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p2.1 0 HotInNvme0n1p1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p3.1 0 Nvme1n1p1
    vms_setup
    vm_run_with_arg 0 1
    vms_prepare "0 1"

    vm_check_scsi_location "0"
    local disks="$SCSI_DISK"
    traddr=""
    get_traddr "Nvme0"
    prepare_fio_cmd_tc1 "0 1"
    $run_fio &
    local last_pid=$!
    sleep 3
    delete_nvme "HotInNvme0n1"
    set +xe
    wait $last_pid
    check_fio_retcode "Scsi hotremove test case 2: Iteration 1." 1 $?
    vm_check_scsi_location "0"
    local new_disks="$SCSI_DISK"
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "0 1"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 2: Iteration 2." 1 $?
    set -xe
    vm_shutdown_all
    add_nvme "HotInNvme1" "$traddr"
    sleep 1
}

function scsi_hotremove_tc3() {
    echo "Scsi hotremove test case 3"
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 HotInNvme1n1p0
    vm_run_with_arg 0 1
    vms_prepare "0 1"
    vm_check_scsi_location "0"
    local disks="$SCSI_DISK"
    traddr=""
    get_traddr "Nvme0"
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    local last_pid=$!
    sleep 3
    delete_nvme "HotInNvme1n1"
    set +xe
    wait $last_pid
    check_fio_retcode "Scsi hotremove test case 3: Iteration 1." 1 $?
    vm_check_scsi_location "0"
    local new_disks="$SCSI_DISK"
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "0 1"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 3: Iteration 2." 1 $?
    set -xe
    vm_shutdown_all
    add_nvme "HotInNvme2" "$traddr"
    sleep 1
}

function scsi_hotremove_tc4() {
    echo "Scsi hotremove test case 4"
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 HotInNvme2n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p2.1 0 HotInNvme2n1p1
    vm_run_with_arg 0 1
    vms_prepare "0 1"

    vm_check_scsi_location "0"
    local disks_vm0="$SCSI_DISK"
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid_vm0=$!
    vm_check_scsi_location "1"
    local disks_vm1="$SCSI_DISK"
    prepare_fio_cmd_tc1 "1"
    $run_fio &
    local last_pid_vm1=$!
    prepare_fio_cmd_tc1 "0 1"
    sleep 3
    traddr=""
    get_traddr "Nvme0"
    delete_nvme "HotInNvme2n1"
    set +xe
    wait $last_pid_vm0
    local retcode_vm0=$?
    wait $last_pid_vm1
    local retcode_vm1=$?
    check_fio_retcode "Scsi hotremove test case 4: Iteration 1." 1 $retcode_vm0

    vm_check_scsi_location "0"
    local new_disks_vm0="$SCSI_DISK"
    check_disks "$disks_vm0" "$new_disks_vm0"
    check_fio_retcode "Scsi hotremove test case 4: Iteration 2." 1 $retcode_vm1
    vm_check_scsi_location "1"
    local new_disks_vm1="$SCSI_DISK"
    check_disks "$disks_vm1" "$new_disks_vm1"

    reboot_all_and_prepare "0 1"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 4: Iteration 3." 1 $?
    prepare_fio_cmd_tc1 "0 1"
    $run_fio
    check_fio_retcode "Scsi hotremove test case 4: Iteration 4." 0 $?
    set -xe
    vm_shutdown_all
    add_nvme "HotInNvme3" "$traddr"
    sleep 1
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p1.0 0
    $rpc_py remove_vhost_scsi_target naa.Nvme0n1p3.1 0
}

function pre_scsi_hotremove_test_case() {
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p2.1
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p3.1
}

function post_scsi_hotremove_test_case() {
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
}

trap "" ERR
pre_scsi_hotremove_test_case
scsi_hotremove_tc1
scsi_hotremove_tc2
scsi_hotremove_tc3
scsi_hotremove_tc4
post_scsi_hotremove_test_case
trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
