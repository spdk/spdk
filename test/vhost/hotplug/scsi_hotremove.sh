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
    new_disks=""
    get_disks "4" new_disks
    check_disks "$disks" "$new_disks"

    bind_nvme "$traddr"
    reboot_all_and_prepare "4"
    get_disks "4" new_disks
}

function hotremove_tc2() {
    echo "Hotremove test case 2"
}

hotremove_tc1
hotremove_tc2
#disk=""
#get_disks 1 disks
#echo $disks
#unbind_nvme 0000:08:00.0
#bind_nvme 0000:08:00.0
