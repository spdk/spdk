#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin="fio"
fio_jobs="$COMMON_DIR/fio_jobs/"
test_type=spdk_vhost_scsi
reuse_vms=false
force_build=false
vms=()
used_vms=""
disk_split=""
x=""

NAME_DISK="NVMe0"

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
			qemu-src=*) QEMU_SRC_DIR="${OPTARG#*=}" ;;
			dpdk-src=*) DPDK_SRC_DIR="${OPTARG#*=}" ;;
			fio-jobs=*) fio_jobs="${OPTARG#*=}" ;;
			dry-run) dry_run=true ;;
			no-shutdown) no_shutdown=true ;;
			test-type=*) test_type="${OPTARG#*=}" ;;
			force-build) force_build=true ;;
			vm=*) vms+=("${OPTARG#*=}") ;;
			disk-split=*) disk_split="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done
shift $(( OPTIND - 1 ))

. $COMMON_DIR/common.sh
. $BASE_DIR/common_hotfeatures.sh


function scsi_test_case1(){
    echo "Test Case 1 SCSI"
    gen_tc_1_scsi_config_and_run_vhost
    run_vhost
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    sleep 5
    back_configuration
}

function scsi_test_case2() {
    echo "Test Case 2 SCSI"
    gen_tc_2_scsi_config_and_run_vhost
    run_vhost
    setup_and_run_vms
    prepare_vms
    check_lsblk_in_vm
    sleep 0.1
	prepare_fio_cmd_tc1 "0"
	$run_fio &
	sleep 5
    bdf="$(get_nvme_pci_addr $COMMON_DIR/vhost.conf.in $NAME_DISK)"
    echo $bdf>/sys/bus/pci/devices/$bdf/driver/unbind
    if check_lsblk_in_vm; then
        echo "Error: NVMe disk is found"
    fi
    reboot_all_vms
	if check_lsblk_in_vm; then
        echo "Error: NVMe disk is found"
    fi
    if $run_fio; then
        echo "Error: Fio working properly"
    fi
    back_configuration
}


## Start  scsi test
check_qemu
check_spdk
scsi_test_case1
scsi_test_case2

