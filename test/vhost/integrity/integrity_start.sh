#!/usr/bin/env bash
set -e

INTEGRITY_BASE_DIR=$(readlink -f $(dirname $0))
ctrl_type="spdk_vhost_scsi"
vm_fs="ext4"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --work-dir=WORK_DIR   Workspace for the test to run"
	echo "    --ctrl-type=TYPE      Controller type to use for test:"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "    --fs=FS_LIST          Filesystems to use for test in VM:"
	echo "                          Example: --fs=\"ext4 ntfs ext2\""
	echo "                          Default: ext4"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "-x                        set -x for script debug"
	exit 0
}

function clean_lvol_cfg()
{
	notice "Removing lvol bdev and lvol store"
	$rpc_py destroy_lvol_bdev lvol_store/lvol_bdev
	$rpc_py destroy_lvol_store -l lvol_store
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
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1
rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR

# Try to kill if any VM remains from previous runs
vm_kill_all

notice "Starting SPDK vhost"
spdk_vhost_run
notice "..."

# Set up lvols and vhost controllers
trap 'clean_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR
# Create the LVS from a Raid-0 bdev, which is created from two Nvme bdevs
notice "Constructing lvol store and lvol bdev on raid-0 bdev which is created from Nvme0n1 and Nvme1n1"
$rpc_py construct_raid_bdev -n raid0 -s 64 -r 0 -b "Nvme0n1 Nvme1n1"
lvs_uuid=$($rpc_py construct_lvol_store raid0 lvol_store)
$rpc_py construct_lvol_bdev lvol_bdev 10000 -l lvol_store

if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1.0
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1.0 0 lvol_store/lvol_bdev
elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1.0 lvol_store/lvol_bdev
fi

# Set up and run VM
setup_cmd="vm_setup --disk-type=$ctrl_type --force=0"
setup_cmd+=" --os=/home/sys_sgsw/vhost_vm_image.qcow2"
setup_cmd+=" --disks=Nvme0n1"
$setup_cmd

# Run VM
vm_run 0
vm_wait_for_boot 600 0

# Run tests on VM
vm_scp 0 $INTEGRITY_BASE_DIR/integrity_vm.sh root@127.0.0.1:/root/integrity_vm.sh
vm_ssh 0 "~/integrity_vm.sh $ctrl_type \"$vm_fs\""

notice "Shutting down virtual machine..."
vm_shutdown_all

clean_lvol_cfg

$rpc_py destroy_raid_bdev raid0
$rpc_py delete_nvme_controller Nvme0
$rpc_py delete_nvme_controller Nvme1

notice "Shutting down SPDK vhost app..."
spdk_vhost_kill
