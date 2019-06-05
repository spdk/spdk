#!/usr/bin/env bash

set -e
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
# Tested with windows vm with OS Name: Microsoft Windows Server 2012 R2 Datacenter
# and OS Version: 6.3.9600 N/A Build 9600
# In order to run this test with windows vm
# windows virtio scsi driver must be installed
WINDOWS_IMG="/home/sys_sgsw/windows_scsi_compliance/windows_vm_image.qcow2"
aio_file="$testdir/aio_disk"
ssh_pass=""
vm_num=1
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

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

trap "rm -f $aio_file; rm -rf $testdir/results; error_exit" SIGINT SIGTERM ERR

mkdir -p $testdir/results
dd if=/dev/zero of=$aio_file bs=1M count=512

timing_enter vhost_run
vhost_run
$rpc_py set_bdev_nvme_hotplug -e
$rpc_py construct_malloc_bdev 256 4096 -b Malloc0
$rpc_py construct_aio_bdev $aio_file Aio0 512
$rpc_py get_bdevs
$rpc_py construct_vhost_scsi_controller naa.vhost.1
$rpc_py add_vhost_scsi_lun naa.vhost.1 0 Nvme0n1
$rpc_py add_vhost_scsi_lun naa.vhost.1 1 Malloc0
# TODO: Currently there is bug for aio device. Disable this test
# $rpc_py add_vhost_scsi_lun naa.vhost.1 2 Aio0
timing_exit vhost_run

timing_enter start_vm
vm_setup --force=1 --disk-type=spdk_vhost_scsi --os=$WINDOWS_IMG --disks=vhost --memory=4096
vm_run "1"
# Wait until VM goes up
vm_wait_for_boot "300" "$vm_num"
timing_exit start_vm

vm_scp "$vm_num" $testdir/windows_scsi_compliance.ps1 127.0.0.1:/cygdrive/c/SCSI/
vm_sshpass "$vm_num" "$ssh_pass" "cd /cygdrive/c/SCSI; powershell.exe -file windows_scsi_compliance.ps1"
vm_scp "$vm_num" 127.0.0.1:/cygdrive/c/SCSI/WIN_SCSI_* $testdir/results/
dos2unix $testdir/results/WIN_SCSI_*.log

notice "Kill vm 1"
vm_kill "$vm_num"
notice "Kill spdk"
vhost_kill
notice "Remove $aio_file"
rm -f $aio_file

python3 $testdir/windows_scsi_compliance.py
rm -rf $testdir/results
