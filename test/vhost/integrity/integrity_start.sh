#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

ctrl_type="spdk_vhost_scsi"
vm_fs="ext4"

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --ctrl-type=TYPE      Controller type to use for test:"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "    --fs=FS_LIST          Filesystems to use for test in VM:"
	echo "                          Example: --fs=\"ext4 ntfs ext2\""
	echo "                          Default: ext4"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "-x                        set -x for script debug"
	exit 0
}

function clean_lvol_cfg() {
	notice "Removing lvol bdev and lvol store"
	$rpc_py bdev_lvol_delete lvol_store/lvol_bdev
	$rpc_py bdev_lvol_delete_lvstore -l lvol_store
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
				fs=*) vm_fs="${OPTARG#*=}" ;;
				*) usage $0 "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage $0 ;;
		x)
			set -x
			x="-x"
			;;
		*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

vhosttestinit

. $(readlink -e "$(dirname $0)/../common.sh") || exit 1
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR

# Try to kill if any VM remains from previous runs
vm_kill_all

notice "Starting SPDK vhost"
vhost_run -n 0
notice "..."

# Set up lvols and vhost controllers
trap 'clean_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR
notice "Creating lvol store and lvol bdev on top of Nvme0n1"
lvs_uuid=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvol_store)
$rpc_py bdev_lvol_create lvol_bdev 10000 -l lvol_store

if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
	$rpc_py vhost_create_scsi_controller naa.Nvme0n1.0
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1.0 0 lvol_store/lvol_bdev
elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
	$rpc_py vhost_create_blk_controller naa.Nvme0n1.0 lvol_store/lvol_bdev
fi

# Set up and run VM
setup_cmd="vm_setup --disk-type=$ctrl_type --force=0"
setup_cmd+=" --os=$VM_IMAGE"
setup_cmd+=" --disks=Nvme0n1"
$setup_cmd

# Run VM
vm_run 0
vm_wait_for_boot 300 0

# Run tests on VM
vm_scp 0 $testdir/integrity_vm.sh root@127.0.0.1:/root/integrity_vm.sh
vm_exec 0 "/root/integrity_vm.sh $ctrl_type \"$vm_fs\""

notice "Shutting down virtual machine..."
vm_shutdown_all

clean_lvol_cfg

$rpc_py bdev_nvme_detach_controller Nvme0

notice "Shutting down SPDK vhost app..."
vhost_kill 0

vhosttestfini
