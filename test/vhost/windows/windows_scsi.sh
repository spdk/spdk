#!/usr/bin/env bash

set -e
WIN_SCSI_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $WIN_SCSI_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $WIN_SCSI_DIR/../../..)
WINDOWS_IMG="/home/sys_sgsw/windows_scsi_compliance/windows_vm_image.qcow2"
. $WIN_SCSI_DIR/../common/common.sh
rpc_py="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

function exit_on_error() {
	set +e
	echo "Error on $1 - $2"
	vm_shutdown_all
	spdk_vhost_kill
	print_backtrace
	exit 1
}

trap "exit_on_error; exit 1" SIGINT SIGTERM EXIT

mkdir -p $WIN_SCSI_DIR/results

timing_enter spdk_vhost_run
spdk_vhost_run
$rpc_py set_bdev_nvme_hotplug -e
$rpc_py construct_malloc_bdev 256 4096 -b Malloc0
$rpc_py construct_malloc_bdev 256 4096 -b Malloc1
$rpc_py construct_split_vbdev Malloc1 2
$rpc_py get_bdevs
$rpc_py construct_vhost_scsi_controller naa.vhost.1
$rpc_py add_vhost_scsi_lun naa.vhost.1 0 Nvme0n1
$rpc_py add_vhost_scsi_lun naa.vhost.1 1 Malloc0
$rpc_py add_vhost_scsi_lun naa.vhost.1 2 Malloc1p1
timing_exit spdk_vhost_run

timing_enter start_vm
vm_setup --force=1 --disk-type=spdk_vhost_scsi --os=$WINDOWS_IMG --disks=vhost --memory=4096
vm_run "1"
vm_wait_for_boot 300 "1"
echo "INFO: VM booted; Waiting a while and starting tests"
timing_exit start_vm

sshpass -p '1234' ssh -p 10100 root@127.0.0.1 "cd /cygdrive/c/SCSI; powershell.exe -file compliance_test.ps1"
sshpass -p '1234' scp -P 10100 root@127.0.0.1:/cygdrive/c/SCSI/WIN_SCSI_* $WIN_SCSI_DIR/results/
dos2unix $WIN_SCSI_DIR/results/WIN_SCSI_*.log

vm_shutdown_all
spdk_vhost_kill
