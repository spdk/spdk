#!/usr/bin/env bash

set -e
WIN_SCSI_DIR=$(readlink -f $(dirname $0))
WINDOWS_IMG="/home/sys_sgsw/windows_scsi_compliance/windows_vm_image.qcow2"
. $WIN_SCSI_DIR/common.sh
aio_file="$WIN_SCSI_DIR/aio_disk"
ssh_pass=""
vm_num=1

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Windows Server scsi compliance test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo "  --vm-ssh-pass=PASSWORD    Text password for the VM"
	echo "  --vm-image-path           Path of windows image"

	exit 0
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			vm-ssh-pass=*) ssh_pass="${OPTARG#*=}" ;;
			vm-image-path=*) WINDOWS_IMG="${OPTARG#*=}" ;;
		esac
		;;
	h) usage $0 ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

trap "rm -f $aio_file; rm -rf $WIN_SCSI_DIR/results; error_exit" SIGINT SIGTERM ERR

mkdir -p $WIN_SCSI_DIR/results
dd if=/dev/zero of=$aio_file bs=1M count=512

timing_enter spdk_vhost_run
spdk_vhost_run
$rpc_py set_bdev_nvme_hotplug -e
$rpc_py construct_malloc_bdev 256 4096 -b Malloc0
$rpc_py construct_aio_bdev $aio_file Aio0 512
$rpc_py get_bdevs
$rpc_py construct_vhost_scsi_controller naa.vhost.1
$rpc_py add_vhost_scsi_lun naa.vhost.1 0 Nvme0n1
$rpc_py add_vhost_scsi_lun naa.vhost.1 1 Malloc0
#$rpc_py add_vhost_scsi_lun naa.vhost.1 2 Aio0
timing_exit spdk_vhost_run

timing_enter start_vm
vm_setup --force=1 --disk-type=spdk_vhost_scsi --os=$WINDOWS_IMG --disks=vhost --memory=4096
vm_run "1"
# Wait until VM goes up
vm_wait_for_boot "300" "$vm_num"
timing_exit start_vm

vm_scp "$vm_num" $WIN_SCSI_DIR/windows_scsi_compliance.ps1 127.0.0.1:/cygdrive/c/SCSI/
vm_sshpass "$vm_num" "$ssh_pass" "cd /cygdrive/c/SCSI; powershell.exe -file windows_scsi_compliance.ps1"
vm_scp "$vm_num" 127.0.0.1:/cygdrive/c/SCSI/WIN_SCSI_* $WIN_SCSI_DIR/results/
dos2unix $WIN_SCSI_DIR/results/WIN_SCSI_*.log

notice "Kill vm 1"
vm_kill "$vm_num"
notice "Kill spdk"
spdk_vhost_kill
notice "Remove $aio_file"
rm -f $aio_file

python3 $WIN_SCSI_DIR/windows_scsi.py
rm -rf $WIN_SCSI_DIR/results
