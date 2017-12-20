set -xe

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

function remove_vhost_controllers() {
    $rpc_py remove_vhost_controller naa.Nvme0n1p0.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p1.0
    $rpc_py remove_vhost_controller naa.Nvme0n1p2.1
    $rpc_py remove_vhost_controller naa.Nvme0n1p3.1
}

function blk_hotremove_tc1() {
    echo "Blk hotremove test case 1"
    traddr=""
    get_traddr "Nvme0"
    unbind_nvme "Nvme0n1"
    sleep 1
    bind_nvme "HotInNvme0" "$traddr"
    sleep 1
}

function blk_hotremove_tc2() {
    echo "Blk hotremove test case 2"
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme0n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 Nvme1n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme1n1p2
    vm_run_with_arg "0 1"
    vms_prepare "0"

    traddr=""
    get_traddr "Nvme0"
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid1=$!
    sleep 3
    unbind_nvme "HotInNvme0n1"
    wait $last_pid1 || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 2: Iteration 1." 1 $retcode

    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 2: Iteration 2." 1 $retcode
    vm_shutdown_all
    remove_vhost_controllers
    bind_nvme "HotInNvme1" "$traddr"
    sleep 1
}

function blk_hotremove_tc3() {
    echo "Blk hotremove test case 3"
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 HotInNvme1n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme1n1p1
    vm_run_with_arg "0 1"
    vms_prepare "0 1"

    traddr=""
    get_traddr "Nvme0"
    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "HotInNvme1n1"
    wait $last_pid || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 3: Iteration 1." 1 $retcode

    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 3: Iteration 2." 1 $retcode
    vm_shutdown_all
    remove_vhost_controllers
    bind_nvme "HotInNvme2" "$traddr"
    sleep 1
}

function blk_hotremove_tc4() {
    echo "Blk hotremove test case 4"
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme2n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 HotInNvme2n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme1n1p1
    vm_run_with_arg "0 1"
    vms_prepare "0 1"

    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid_vm0=$!

    prepare_fio_cmd_tc1 "1"
    $run_fio &
    last_pid_vm1=$!

    sleep 3
    unbind_nvme "HotInNvme2n1"
    wait $last_pid_vm0 || retcode_vm0=$? || true
    wait $last_pid_vm1 || retcode_vm1=$? || true
    check_fio_retcode "Blk hotremove test case 4: Iteration 1." 1 $retcode_vm0
    check_fio_retcode "Blk hotremove test case 4: Iteration 2." 1 $retcode_vm1

    reboot_all_and_prepare "0 1"
    $run_fio|| retcode=$? || true
    check_fio_retcode "Blk hotremove test case 4: Iteration 3." 1 $retcode

    vm_shutdown_all
    remove_vhost_controllers
    bind_nvme "HotInNvme3" "$traddr"
    sleep 1
}

function blk_hotremove_tc5() {
    echo "Blk hotremove test case 5"
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 HotInNvme3n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.1 Nvme1n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme1n1p2
    vm_run_with_arg "0 1"
    vms_prepare "0 1"

    prepare_fio_cmd_tc1 "0"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "HotInNvme3n1"
    wait $last_pid || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 5: Iteration 1." 1 $retcode
    reboot_all_and_prepare "0"
    $run_fio || retcode=$? || true
    check_fio_retcode "Blk hotremove test case 5: Iteration 2." 1 $retcode
    vm_shutdown_all
    remove_vhost_controllers
    bind_nvme "HotInNvme4" "$traddr"
    sleep 1
}

trap "" ERR
vms_setup
blk_hotremove_tc1
blk_hotremove_tc2
blk_hotremove_tc3
blk_hotremove_tc4
blk_hotremove_tc5
trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
