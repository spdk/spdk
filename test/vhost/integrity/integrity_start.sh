#!/usr/bin/env bash
set -e

rootdir=$(readlink -f $(dirname $0))/../../..
source "$rootdir/scripts/common.sh"

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$SPDK_DIR" ]] && SPDK_DIR="$(cd $BASE_DIR/../../../ && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"

. $COMMON_DIR/common.sh
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py "

ctrl_type="spdk_vhost_scsi"
vm_fs="ext4"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
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

function clean_lvol_cfg()
{
	notice "Removing lvol bdev and lvol store"
	$rpc_py delete_bdev lvol_store/lvol_bdev
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

trap 'error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR

# Try to kill if any VM remains from previous runs
vm_kill_all

notice "Starting SPDK vhost"
spdk_vhost_run $BASE_DIR
notice "..."

# Set up lvols and vhost controllers
trap 'clean_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR
notice "Constructing lvol store and lvol bdev on top of Nvme0n1"
lvs_uuid=$($rpc_py construct_lvol_store Nvme0n1 lvol_store)
$rpc_py construct_lvol_bdev lvol_bdev $(get_lvs_free_mb $lvs_uuid) -l lvol_store

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
if [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
	setup_cmd+="_size_$(get_bdev_size lvol_store/lvol_bdev)"
fi
$setup_cmd

# Run VM
vm_run "0"
vm_wait_for_boot 600 "0"

# Pack current SPDK sources
cd $SPDK_DIR/..
tar --exclude="*.o" --exclude="*.d" --exclude="*.git" -C $SPDK_DIR -zcf spdk.tar.gz .
mv spdk.tar.gz $SPDK_DIR/
cd $SPDK_DIR
trap "rm -f $SPDK_DIR/spdk.tar.gz; error_exit ${FUNCNAME} ${LINENO}" SIGTERM SIGABRT ERR

# Run tests on VM
vm_scp "0" $BASE_DIR/integrity_vm.sh root@127.0.0.1:/root/integrity_vm.sh
vm_scp "0" $SPDK_DIR/spdk.tar.gz root@127.0.0.1:/root/spdk.tar.gz
vm_ssh "0" "fs=\"$vm_fs\" ~/integrity_vm.sh $ctrl_type"

notice "Shutting down virtual machine..."
vm_shutdown_all
sleep 2

clean_lvol_cfg
notice "Shutting down SPDK vhost app..."
spdk_vhost_kill
rm -f $SPDK_DIR/spdk.tar.gz
