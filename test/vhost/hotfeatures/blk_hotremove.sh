#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh
function get_first_disk() {
    vm_check_blk_location $1
    disk_array=( $SCSI_DISK )
    eval "$2=${disk_array[0]}"
}
function prepare_fio() {
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_job
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_blk_location $vm_num
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]" >> $tmp_job
            echo "filename=/dev/$disk" >> $tmp_job
        done
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_4discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
        rm $tmp_job
    done
}

function gen_tc_1_blk_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in
    echo "HotplugEnable Yes" >> $BASE_DIR/vhost.conf.in
    echo "[Split]" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 1" >> $BASE_DIR/vhost.conf.in

    echo "[VhostBlk0]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
}
function gen_tc_2_blk_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in
    echo "  HotplugEnable Yes" >> $BASE_DIR/vhost.conf.in
    echo "[Split]" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 1" >> $BASE_DIR/vhost.conf.in

    echo "[VhostBlk0]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p0" >> $BASE_DIR/vhost.conf.in
}

function gen_tc_3_blk_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in
    echo "  HotplugEnable Yes" >> $BASE_DIR/vhost.conf.in
    echo "[Split]" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 1" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme1n1 1" >> $BASE_DIR/vhost.conf.in

    echo "[VhostBlk0]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p0" >> $BASE_DIR/vhost.conf.in
    echo "[VhostBlk1]" >> $BASE_DIR/vhost.conf.in
    echo "  Name  naa.Nvme1n1p0.1" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme1n1p0" >> $BASE_DIR/vhost.conf.in
}

function gen_tc_4_blk_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in
    echo "  HotplugEnable Yes" >> $BASE_DIR/vhost.conf.in
    echo "[Split]" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 2" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme1n1 2" >> $BASE_DIR/vhost.conf.in

    echo "[VhostBlk0]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p0" >> $BASE_DIR/vhost.conf.in
    echo "[VhostBlk1]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p1.1" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p1" >> $BASE_DIR/vhost.conf.in
    echo "[VhostBlk2]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme1n1p0.1" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme1n1p0" >> $BASE_DIR/vhost.conf.in
    echo "[VhostBlk3]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme1n1p1.0" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme1n1p1" >> $BASE_DIR/vhost.conf.in
}

function gen_tc_5_blk_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf.in
    echo "  HotplugEnable Yes" >> $BASE_DIR/vhost.conf.in
    echo "[Split]" >> $BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 2" >> $BASE_DIR/vhost.conf.in

    echo "[VhostBlk0]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p0.0" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p0" >> $BASE_DIR/vhost.conf.in
    echo "[VhostBlk1]" >> $BASE_DIR/vhost.conf.in
    echo "  Name naa.Nvme0n1p1.1" >> $BASE_DIR/vhost.conf.in
    echo "  Dev Nvme0n1p1" >> $BASE_DIR/vhost.conf.in
}

function blk_test_case1(){
    echo "Test Case 1 blk"
    gen_tc_1_blk_config
    run_vhost
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme0")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    sleep 5
    back_configuration
}

function blk_test_case2() {
    echo "Test Case 2 blk"
    gen_tc_2_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 5
    prepare_fio 0
    $run_fio &
    first_fio_pid=$!
    sleep 5
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +ex
    check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1
    vms_reboot_all
    vms_prepare
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 2: After reboot vm." 1
    back_configuration
}

function blk_test_case3() {
    echo "Test Case 3 blk"
    gen_tc_3_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 0.1
    prepare_fio $used_vms
    $run_fio &
    sleep 5
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After unbind disk." 1
    vms_reboot_all
    vms_prepare
    $run_fio
    check_fio_retcode "BLK HOT REMOVE test case 3: After reboot vm." 1
    back_configuration
}

function blk_test_case4() {
    echo "Test Case 4 blk"
    gen_tc_4_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 5
    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        if [[ $vm_num == 0 ]]; then
           fio_pid=$!
        fi
        sleep 5
    done
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme1")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." $vm_num $!
    wait $fio_pid
    check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." 1
    vms_reboot_all
    vms_prepare
    for vm_num in $used_vms; do
        $run_fio
        if [[ $vm_num == 0 ]]; then
           fio_pid=$!
           wait $fio_pid
        else
           check_fio_retcode "BLK HOT REMOVE test case 4: After reboot vm." $vm_num $!
        fi
    done
    back_configuration
}

function blk_test_case5() {
    echo "Test Case 5 blk"
    gen_tc_5_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 5
    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        sleep 5
    done
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in $nvme_disk)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    for vm_num in $used_vms; do
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1
    done
    vms_reboot_all
    vms_prepare
    for vm_num in $used_vms; do
        $run_fio &
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1
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
