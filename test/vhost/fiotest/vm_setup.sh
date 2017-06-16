#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for setting up VMs for tests"
	echo "Usage: $(basename $1) [OPTIONS] VM_NUM"
	echo
	echo "-h, --help                print help and exit"
	echo "-f VM_NUM                 Force VM_NUM reconfiguration if already exist"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exit. (default: $TEST_DIR)"
	echo "    --test-type=TYPE      Perform specified test:"
	echo "                          virtio - test host virtio-scsi-pci using file as disk image"
	echo "                          kernel_vhost - use kernel driver vhost-scsi"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "    ---cache=CACHE        Use CACHE for virtio test: "
	echo "                          writethrough, writeback, none, unsafe or directsyns"
	echo "                          Default is writethrough"
	echo "    --disk=PATH           Disk to use in test. test specific meaning:"
	echo "                          virtio - disk path (file or block device ex: /dev/nvme0n1)"
	echo "                          kernel_vhost - the WWN number to be used"
	echo "                          spdk_vhost - the socket path. Default is WORK_DIR/vhost/usvhost"
	echo "    --os=OS_QCOW2         Custom OS qcow2 image file"
	echo "    --os-mode=MODE        MODE how to use provided image: default: backing"
	echo "                          backing - create new image but use provided backing file"
	echo "                          copy - copy provided image and use a copy"
	echo "                          orginal - use file directly. Will modify the provided file"
	echo "-x                        Turn on script debug (set -x)"
	exit 0
}
disk=""
raw_cache=""
img_mode=""
os=""
while getopts 'xf:h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			raw-cache=*) raw_cache="--raw-cache=${OPTARG#*=}" ;;
			test-type=*) test_type="${OPTARG#*=}" ;;
			spdk-vhost-mode=*) spdk_vhost_mode="${OPTARG#*=}" ;;
			disk=*) disk="${OPTARG#*=}" ;;
			os=*) os="${OPTARG#*=}"
				if [[ ! -r "$os" ]]; then
					echo "ERROR: can't read '$os'"
					usage $0
				fi
				os="$(readlink -f $os)"
				;;
			os-mode=*) os_mode="--os-mode=${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0	;;
	x) set -x ;;
	f) force_vm_num="--force=${OPTARG#*=}" ;;
	*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

. $BASE_DIR/common.sh

[[ -z "$os" ]] && os="$TEST_DIR/debian.qcow2"
[[ $test_type =~ "spdk_vhost" ]] && [[ -z "$disk" ]] && disk="$SPDK_VHOST_SCSI_TEST_DIR/usvhost"
if [[ $test_type == "kernel_vhost" ]] && [[ -z "$disk" ]]; then
	echo "ERROR: for $test_type '--disk=WWN' is mandatory"
	exit 1
fi

vm_setup \
	--os=$os \
	--disk-type=$test_type \
	--disks=$disk \
	$wwn $raw_cache $force_vm_num $os_mode
