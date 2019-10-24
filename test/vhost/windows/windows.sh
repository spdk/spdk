#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
ctrl_type="spdk_vhost_scsi"
ssh_pass=""
vm_num="0"
vm_image="/home/sys_sgsw/windows_server.qcow2"

function usage()
{
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Windows Server automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo "--vm-ssh-pass=PASSWORD    Text password for the VM"
	echo "--vm-image=PATH           Path to qcow2 image of Windows VM"
	echo "--ctrl-type=TYPE          Controller type to use for test:"
	echo "                              spdk_vhost_scsi - use spdk vhost scsi"
	echo "                              spdk_vhost_blk - use spdk vhost block"
	echo "-x                        set -x for script debug"
	echo "-h, --help                Print help and exit"

	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			vm-ssh-pass=*) ssh_pass="${OPTARG#*=}" ;;
			vm-image=*) vm_image="${OPTARG#*=}" ;;
			ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

# For some reason there is a problem between using SSH key authentication
# and Windows UAC. Some of the powershell commands fail due to lack of
# permissons, despite script running in elevated mode.
# There are some clues about this setup that suggest this might not work properly:
# https://superuser.com/questions/181581/how-can-i-run-something-as-administrator-via-cygwins-ssh
# https://cygwin.com/ml/cygwin/2004-09/msg00087.html
# But they apply to rather old Windows distributions.
# Potentially using Windows Server 2016 and newer may solve the issue
# due to OpenSSH being available directly from Windows Store.
function vm_sshpass()
{
	vm_num_is_valid $1 || return 1

	local ssh_cmd
	ssh_cmd="sshpass -p $2 ssh \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no \
		-o User=root \
		-p $(vm_ssh_socket $1) $VM_SSH_OPTIONS 127.0.0.1"

	shift 2
	$ssh_cmd "$@"
}

if [[ -z "$ssh_pass" ]]; then
	error "Please specify --vm-ssh-pass parameter"
fi

trap 'error_exit "${FUNCNAME}" "${LINENO}"; rm -f $aio_file' SIGTERM SIGABRT ERR

vm_kill_all

# Run vhost without debug!
# Windows Virtio drivers use indirect descriptors without negotiating
# their feature flag, which is explicitly forbidden by the Virtio 1.0 spec.
# "(2.4.5.3.1 Driver Requirements: Indirect Descriptors)
# The driver MUST NOT set the VIRTQ_DESC_F_INDIRECT flag unless the
# VIRTIO_F_INDIRECT_DESC feature was negotiated.".
# Violating this rule doesn't cause any issues for SPDK vhost,
# but triggers an assert, so we can only run Windows VMs with non-debug SPDK builds.
notice "running SPDK vhost"
vhost_run 0
notice "..."

# Prepare bdevs for later vhost controllers use
# Nvme bdev is automatically constructed during vhost_run
# by using scripts/gen_nvme.sh. No need to add it manually.
# Using various sizes to better identify bdevs if no name in BLK
# is available
# TODO: use a param for blocksize for AIO and Malloc bdevs
aio_file="$testdir/aio_disk"
dd if=/dev/zero of=$aio_file bs=1M count=512
$rpc_py bdev_aio_create $aio_file Aio0 512
$rpc_py bdev_malloc_create -b Malloc0 256 512
$rpc_py bdev_get_bdevs

# Create vhost controllers
# Prepare VM setup command
setup_cmd="vm_setup --force=0 --memory=8192"
setup_cmd+=" --os=$vm_image"

if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
	$rpc_py vhost_create_scsi_controller naa.0.0
	$rpc_py vhost_scsi_controller_add_target naa.0.0 0 Nvme0n1
	$rpc_py vhost_scsi_controller_add_target naa.0.0 1 Malloc0
	$rpc_py vhost_scsi_controller_add_target naa.0.0 2 Aio0
	setup_cmd+=" --disk-type=spdk_vhost_scsi --disks=0"
elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
	$rpc_py vhost_create_blk_controller naa.0.0 Nvme0n1
	$rpc_py vhost_create_blk_controller naa.1.0 Malloc0
	$rpc_py vhost_create_blk_controller naa.2.0 Aio0
	setup_cmd+=" --disk-type=spdk_vhost_blk --disks=0:1:2"
fi
$rpc_py vhost_get_controllers
$setup_cmd

# Spin up VM
vm_run "$vm_num"
vm_wait_for_boot "300" "$vm_num"

vm_sshpass "$vm_num" "$ssh_pass" "mkdir /cygdrive/c/fs_test"
vm_scp "$vm_num" "$testdir/windows_fs_test.ps1" "127.0.0.1:/cygdrive/c/fs_test"
vm_sshpass "$vm_num" "$ssh_pass" "cd /cygdrive/c/fs_test; powershell.exe -file windows_fs_test.ps1"

notice "Shutting down Windows VM..."
# Killing, actually. #TODO: implement vm_windwows_shutdown() function
vm_kill $vm_num

notice "Shutting down SPDK vhost app..."
vhost_kill 0

rm -f $aio_file
