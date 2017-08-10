#!/usr/bin/env bash
set -ex
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
        echo "==============="
        echo ""
        echo "INFO: Testing..."

        echo "INFO: Running fio jobs ..."
        run_fio="python $COMMON_DIR/run_fio.py "
        run_fio+="$fio_bin "
        run_fio+="--job-file="
        for job in $fio_jobs; do
                run_fio+="$job,"
        done
        run_fio="${run_fio::-1}"
        run_fio+=" "
        run_fio+="--out=$TEST_DIR "

        if [[ ! $disk_split == '' ]]; then
                run_fio+="--split-disks=$disk_split "
        fi

        # Check if all VM have disk in tha same location
        DISK=""
        for vm_num in $1; do

                vm_dir=$VM_BASE_DIR/$vm_num
                run_fio+="127.0.0.1:$(cat $vm_dir/fio_socket):"
                get_first_disk "0" first_disk
                for disk in $first_disk; do
                        run_fio+="/dev/$disk:"
                done
                run_fio="${run_fio::-1}"
                run_fio+=","
        done

        run_fio="${run_fio%,}"
        run_fio+=" "
        run_fio="${run_fio::-1}"
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
    vms_prepare
    vms_setup_and_run
    sleep 0.1
    prepare_fio "$used_vms"
    $run_fio &
    first_fio_pid=$!
    sleep 5
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme0")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    wait $first_fio_pid
    $run_fio
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
    vms_prepare
    vms_setup_and_run
    sleep 0.1
    prepare_fio 0
    $run_fio &
    sleep 5
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme0")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    wait $first_fio_pid
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
    fio_pid=[]
    gen_tc_4_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 5
    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        fio_pid[$vm_num]=$!
        sleep 5
    done
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme1")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    for vm_num in $used_vms; do
        wait $[fio_pid[$vm_num]]
        check_fio_retcode "BLK HOT REMOVE test case 4: After unbind disk." $vm_num
    done
    vms_reboot_all
    vms_prepare
    for vm_num in $used_vms; do
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 4: After reboot vm." $vm_num
    done
    back_configuration
}

function blk_test_case5() {
    echo "Test Case 5 blk"
    fio_pid=[]
    gen_tc_5_blk_config
    run_vhost
    vms_setup_and_run
    vms_prepare
    sleep 5
    for vm_num in $used_vms; do
        prepare_fio $vm_num
        $run_fio &
        fio_pid[$vm_num]=$!
        sleep 5
    done
    bdf="$(get_nvme_pci_addr $BASE_DIR/vhost.conf.in "Nvme0")"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    set +xe
    for vm_num in $used_vms; do
        wait $[fio_pid[$vm_num]]
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1
    done
    vms_reboot_all
    vms_prepare
    for vm_num in $used_vms; do
        $run_fio
        check_fio_retcode "BLK HOT REMOVE test case 2: After unbind disk." 1
    done
    back_configuration
}

## Start  blk test
if test_case == "single_vm_one_disk"; then
    blk_test_case1
    blk_test_case2
fi

if test_case == "single_vm_two_disk"; then
    blk_test_case3
fi
if test_case == "multi_vm"; then
    blk_test_case4
    blk_test_case5
fi
