#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function get_disks() {
    vm_check_scsi_location $1
    eval $2='"$SCSI_DISK"'
}

function check_disks() {
    if [ "$1" == "$2" ]; then
        echo "Disk has not been deleted"
        exit 1
    fi
}

function get_traddr() {
    nvme_name=$1
    nvme="$( $BASE_DIR/../../../scripts/gen_nvme.sh )"
    echo $nvme
    while read -r line; do
        if [[ $line == *"TransportID"* ]] && [[ $line == *$nvme_name* ]]; then
            word_array=($line)
            for word in "${word_array[@]}"; do
                if [[ $word == *"traddr"* ]]; then
                    traddr=$( echo $word | sed 's/traddr://' | sed 's/"//' )
                    eval $2=$traddr
                fi
            done
        fi
    done <<< "$nvme"
}

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

function unbind_nvme() {
    echo "$1" > "/sys/bus/pci/devices/$1/driver/unbind"
}

function bind_nvme() {
    echo "$1" > "/sys/bus/pci/drivers/uio_pci_generic/bind"
}

function hotremove_tc1() {
    echo "Hotremove test case 1"
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
    $rpc_py get_bdevs
    #check_disks "$disks" "$new_disks"

    #bind_nvme "$traddr"
    #reboot_all_and_prepare "4"
    #get_disks "4" new_disks
}

function hotremove_tc2() {
    echo "Hotremove test case 2"
    disks=""
    get_disks "4" disks
    prepare_fio_cmd_tc1 "4"
    $run_fio &
    last_pid=$!
    sleep 3
    unbind_nvme "$traddr"
    set +xe
    wait $last_pid
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"
    bind_nvme "$traddr"
    reboot_all_and_prepare "4"
}

hotremove_tc1
hotremove_tc2
#disk=""
#get_disks 1 disks
#echo $disks
#unbind_nvme 0000:08:00.0
#bind_nvme 0000:08:00.0
