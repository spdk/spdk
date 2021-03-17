#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

# Tested with windows vm with OS Name: Microsoft Windows Server 2012 R2 Datacenter
# and OS Version: 6.3.9600 N/A Build 9600
# In order to run this test with windows vm
# windows virtio scsi driver must be installed
WINDOWS_IMG="$DEPENDENCY_DIR/windows_scsi_compliance/windows_vm_image.qcow2"
aio_file="$SPDK_TEST_STORAGE/aio_disk"
ssh_pass=""
vm_num=1
keep_results_dir=false
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Windows Server scsi compliance test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo "  --vm-ssh-pass=PASSWORD    Text password for the VM"
	echo "  --vm-image-path           Path of windows image"
	echo "  --keep_results            Do not delete dir with results"

	exit 0
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				vm-ssh-pass=*) ssh_pass="${OPTARG#*=}" ;;
				vm-image-path=*) WINDOWS_IMG="${OPTARG#*=}" ;;
				keep_results*) keep_results_dir=true ;;
			esac
			;;
		h) usage $0 ;;
		*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

trap 'rm -f $aio_file; rm -rf $testdir/results; error_exit' SIGINT SIGTERM ERR

VM_PASSWORD="$ssh_pass"
mkdir -p $testdir/results
dd if=/dev/zero of=$aio_file bs=1M count=512

timing_enter vhost_run
vhost_run -n 0
$rpc_py bdev_nvme_set_hotplug -e
$rpc_py bdev_malloc_create 256 4096 -b Malloc0
$rpc_py bdev_aio_create $aio_file Aio0 512
$rpc_py bdev_get_bdevs
$rpc_py vhost_create_scsi_controller naa.vhost.1
$rpc_py vhost_scsi_controller_add_target naa.vhost.1 0 Nvme0n1
$rpc_py vhost_scsi_controller_add_target naa.vhost.1 1 Malloc0
# TODO: Currently there is bug for aio device. Disable this test
# $rpc_py vhost_scsi_controller_add_target naa.vhost.1 2 Aio0
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
vhost_kill 0
notice "Remove $aio_file"
rm -f $aio_file

python3 $testdir/windows_scsi_compliance.py
if ! $keep_results_dir; then
	rm -rf $testdir/results
fi
