#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

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
                vm_check_scsi_location $vm_num
                for disk in $SCSI_DISK; do
                        run_fio+="/dev/$disk:"
                done
                run_fio="${run_fio::-1}"
                run_fio+=","
        done

        run_fio="${run_fio%,}"
        run_fio+=" "
        run_fio="${run_fio::-1}"
}

function gen_tc_1_scsi_config() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 1" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
}

function gen_tc_2_scsi_config() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in

	echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 2" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p1.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p1" >> $COMMON_DIR/vhost.conf.in

	cp $COMMON_DIR/autotest.config $COMMON_DIR/autotest.config.tmp
	echo -e "VM_1_qemu_mask=0x2\nVM_1_qemu_numa_node=0">>$COMMON_DIR/autotest.config
}

function gen_tc_3_scsi_config() {
	cp $COMMON_DIR/vhost.conf.in $COMMON_DIR/vhost.conf.tmp
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $COMMON_DIR/vhost.conf.in
    echo "[Split]" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme0n1 1" >> $COMMON_DIR/vhost.conf.in
	echo "  Split Nvme1n1 1" >> $COMMON_DIR/vhost.conf.in

	echo "[VhostScsi0]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name naa.Nvme0n1p0.0" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 0 Nvme0n1p0" >> $COMMON_DIR/vhost.conf.in
	echo "[VhostScsi1]" >> $COMMON_DIR/vhost.conf.in
	echo "  Name  naa.Nvme1n1p0.1" >> $COMMON_DIR/vhost.conf.in
	echo "  Dev 1 Nvme1n1p0" >> $COMMON_DIR/vhost.conf.in

	cp $COMMON_DIR/autotest.config $COMMON_DIR/autotest.config.tmp
	echo -e "VM_1_qemu_mask=0x2\nVM_1_qemu_numa_node=0">>$COMMON_DIR/autotest.config
}

function scsi_test_case1(){
    echo "Test Case 1 SCSI HOTREMOVE"
    gen_tc_1_scsi_config
    run_vhost
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    sleep 5
    back_configuration
}

function scsi_test_case2() {
    echo "Test Case 2 SCSI HOTREMOVE"
    gen_tc_2_scsi_config
    run_vhost
    setup_and_run_vms
    prepare_vms
    DISK_NVME=$(check_lsblk_in_vm)
    sleep 0.1
	prepare_fio_cmd_tc1 "0"
	$run_fio &
	sleep 5
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind

    if ! $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi
    $run_fio
    check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." 1
    reboot_all_vms

	if $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    $run_fio
    check_fio_retcode "SCSI HOT REMOVE test case 3: After reboot." 1
    back_configuration
}

function scsi_test_case3() {
    echo "Test Case 3 SCSI HOTREMOVE"
    gen_tc_3_scsi_config
    run_vhost
    vms_setup_and_run
    prepare_vms
    DISK_NVME=$(check_lsblk_in_vm)
    sleep 0.1
	prepare_fio "used_vms"
	$run_fio &
	sleep 5
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind

    if $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    $run_fio
    check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." 1
    reboot_all_vms

	if $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
        echo "Error: NVMe disk is found"
    fi

    $run_fio
    check_fio_retcode "SCSI HOT REMOVE test case 3: After reboot." 1
    back_configuration
}

function scsi_test_case4() {
    echo "Test Case 4 SCSI HOTREMOVE"
    gen_tc_3_scsi_config
    run_vhost
    vms_setup_and_run
    prepare_vms
    DISK_NVME=$(check_lsblk_in_vm)
    sleep 0.1
	$run_fio &
	sleep 5
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    for vm_num in $used_vms; do
        prepare_fio "vm_num"
        if $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
            echo "Error: NVMe disk is found"
        fi
        $run_fio
        check_fio_retcode "SCSI HOT REMOVE test case 3: After unbind disk." 1
        reboot_all_vms

        if $(vm_ssh $vm_num "lsblk -d /dev/$DISK_NVME>>/dev/null"); then
            echo "Error: NVMe disk is found"
        fi

        $run_fio
        check_fio_retcode "SCSI HOT REMOVE test case 3: After reboot." 1
    done
    back_configuration
}

## Start  scsi test
check_qemu
check_spdk

if test_case == "single_vm_one_disk"; then
    scsi_test_case1
fi

if test_case == "single_vm_two_disk"; then
    scsi_test_case3
fi

if test_case == "multi_vm"; then
    #two split
    scsi_test_case2
     #two disk
    scsi_test_case4
fi

