#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for setting up VMs for tests"
	echo "Usage: $(basename $1) [OPTIONS] VM_NUM"
	echo
	echo "-h, --help                    Print help and exit"
	echo "    --work-dir=WORK_DIR       Where to find build file. Must exit. (default: $TEST_DIR)"
	echo "    --force=VM_NUM            Force VM_NUM reconfiguration if already exist"
	echo "    --disk-type=TYPE          Perform specified test:"
	echo "                              virtio - test host virtio-scsi-pci using file as disk image"
	echo "                              kernel_vhost - use kernel driver vhost-scsi"
	echo "                              spdk_vhost_scsi - use spdk vhost scsi"
	echo "                              spdk_vhost_blk - use spdk vhost block"
	echo "    --raw-cache=CACHE         Use CACHE for virtio test: "
	echo "                              writethrough, writeback, none, unsafe or directsyns"
	echo "    --disk=PATH[,disk_type]   Disk to use in test. test specific meaning:"
	echo "                              virtio - disk path (file or block device ex: /dev/nvme0n1)"
	echo "                              kernel_vhost - the WWN number to be used"
	echo "                              spdk_vhost_[scsi|blk] - the socket path."
	echo "                              optional disk_type - set disk type for disk (overwrites test-type)"
	echo "                              e.g. /dev/nvme0n1,spdk_vhost_scsi"
	echo "    --os=OS_QCOW2             Custom OS qcow2 image file"
	echo "    --os-mode=MODE            MODE how to use provided image: default: backing"
	echo "                              backing - create new image but use provided backing file"
	echo "                              copy - copy provided image and use a copy"
	echo "                              orginal - use file directly. Will modify the provided file"
	echo "    --incoming=VM_NUM         Use VM_NUM as source migration VM."
	echo "    --migrate-to=VM_NUM       Use VM_NUM as target migration VM."
	echo "    --vhost-num=NUM       Optional: vhost instance NUM to be used by this VM. Default: 0"
	echo "-x                            Turn on script debug (set -x)"
	echo "-v                            Be more verbose"
	exit 0
}

setup_params=()
for param in "$@"; do
	case "$param" in
		--help|-h) usage $0 ;;
		--work-dir=*)
			TEST_DIR="${param#*=}"
			continue
			;;
		--raw-cache=*) ;;
		--disk-type=*) ;;
		--disks=*) ;;
		--os=*) ;;
		--os-mode=*) ;;
		--force=*) ;;
		--incoming=*) ;;
		--migrate-to=*) ;;
		-x)
			set -x
			continue
			;;
		-v)
			SPDK_VHOST_VERBOSE=true
			continue
			;;
		*) usage $0 "Invalid argument '$param'" ;;
	esac

	setup_params+=( "$param" )
done

. $COMMON_DIR/common.sh

vm_setup ${setup_params[@]}

trap -- ERR
