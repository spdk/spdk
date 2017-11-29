#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function blk_hotremove_tc1() {
    echo "Blk hotremove test case 1"
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p12.4 Nvme0n1p18
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p13.4 Nvme0n1p19
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
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p12.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p13.4
}

function blk_hotremove_tc2() {
    echo "Blk hotremove test case 2"
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p12.4 HotInNvme0n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p13.4 HotInNvme0n1p1
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
    check_fio_retcode "Blk hotremove test case 2: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Blk hotremove test case 2: Iteration 2." 1 $?
    vm_kill "4"
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p12.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p13.4
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc3() {
    echo "Blk hotremove test case 3"
    $SPDK_BUILD_DIR/scripts/rpc.py get_bdevs
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p12.4 HotInNvme1n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p13.4 HotInNvme1n1p1
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
    check_fio_retcode "Blk hotremove test case 3: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Blk hotremove test case 3: Iteration 2." 1 $?
    vm_kill "4"
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p12.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p13.4
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc4() {
    echo "Blk hotremove test case 4"
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p12.4 HotInNvme0n2p0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p13.4 HotInNvme0n2p1

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
    check_fio_retcode "Blk hotremove test case 4: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Blk hotremove test case 4: Iteration 2." 1 $?
    vm_kill "4"
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p12.4
    $SPDK_BUILD_DIR/scripts/rpc.py remove_vhost_controller naa.Nvme0n1p13.4
    bind_nvme "$traddr"
    sleep 1
}

function blk_hotremove_tc5() {
    echo "Blk hotremove test case 5"
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p12.4 HotInNvme3n1p0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_blk_controller naa.Nvme0n1p13.4 HotInNvme3n1p1
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
    check_fio_retcode "Blk hotremove test case 5: Iteration 1." 1 $?
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    reboot_all_and_prepare "4"
    $run_fio
    check_fio_retcode "Blk hotremove test case 5: Iteration 2." 1 $?
    vm_kill "4"
    bind_nvme "$traddr"
    sleep 1
}

blk_hotremove_tc1
blk_hotremove_tc2
blk_hotremove_tc3
blk_hotremove_tc4
blk_hotremove_tc5
