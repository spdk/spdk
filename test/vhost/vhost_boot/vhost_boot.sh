#!/usr/bin/env bash
set -xe

basedir=$(readlink -f $(dirname $0))
. $basedir/../common/common.sh
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
vm_no="0"

function err_clean
{
	trap - ERR
	print_backtrace
	set +e
	error "Error on $1 $2"
	vm_kill_all
	$rpc_py remove_vhost_scsi_target naa.vhost_vm.$vm_no 0
	$rpc_py remove_vhost_controller naa.vhost_vm.$vm_no
	$rpc_py destroy_lvol_bdev $lvb_u
	$rpc_py destroy_lvol_store -u $lvs_u
	spdk_vhost_kill
	exit 1
}

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) vm_image=PATH [-h|--help]"
	echo "-h, --help            Print help and exit"
	echo "    --vm_image=PATH   Path to VM image used in these tests"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			vm_image=*) os_image="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

if [[ -z $os_image ]]; then
	echo "No path to os image is given"
	exit 1
fi

timing_enter vhost_boot
trap 'err_clean "${FUNCNAME}" "${LINENO}"' ERR
timing_enter start_vhost
spdk_vhost_run
timing_exit start_vhost

timing_enter create_lvol
lvs_u=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
lvb_u=$($rpc_py construct_lvol_bdev -u $lvs_u lvb0 20000)
timing_exit create_lvol

timing_enter convert_vm_image
modprobe nbd
trap '$rpc_py stop_nbd_disk /dev/nbd0; rmmod nbd; err_clean "${FUNCNAME}" "${LINENO}"' ERR
$rpc_py start_nbd_disk $lvb_u /dev/nbd0
$QEMU_PREFIX/bin/qemu-img convert $os_image -O raw /dev/nbd0
sync
$rpc_py stop_nbd_disk /dev/nbd0
sleep 1
rmmod nbd
timing_exit convert_vm_image

trap 'err_clean "${FUNCNAME}" "${LINENO}"' ERR
timing_enter create_vhost_controller
$rpc_py construct_vhost_scsi_controller naa.vhost_vm.$vm_no
$rpc_py add_vhost_scsi_lun naa.vhost_vm.$vm_no 0 $lvb_u
timing_exit create_vhost_controller

timing_enter setup_vm
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --disks="vhost_vm" --spdk-boot="vhost_vm"
vm_run $vm_no
vm_wait_for_boot 600 $vm_no
timing_exit setup_vm

timing_enter run_vm_cmd
vm_ssh $vm_no "parted -s /dev/sda mkpart primary 10GB 100%; partprobe;  sleep 0.1;"
vm_ssh $vm_no "mkfs.ext4 -F /dev/sda2; mkdir -p /mnt/sda2test; mount /dev/sda2 /mnt/sda2test;"
vm_ssh $vm_no "fio --name=integrity --bsrange=4k-512k --iodepth=128 --numjobs=1 --direct=1 \
 --thread=1 --group_reporting=1 --rw=randrw --rwmixread=70 --filename=/mnt/sda2test/test_file \
 --verify=md5 --do_verify=1 --verify_backlog=1024 --fsync_on_close=1 --runtime=20 \
 --time_based=1 --size=1024m"
vm_ssh $vm_no "umount /mnt/sda2test; rm -rf /mnt/sda2test"
alignment_offset=$(vm_ssh $vm_no "cat /sys/block/sda/sda1/alignment_offset")
echo "alignment_offset: $alignment_offset"
timing_exit run_vm_cmd

vm_shutdown_all

timing_enter clean_vhost
$rpc_py remove_vhost_scsi_target naa.vhost_vm.$vm_no 0
$rpc_py remove_vhost_controller naa.vhost_vm.$vm_no
$rpc_py destroy_lvol_bdev $lvb_u
$rpc_py destroy_lvol_store -u $lvs_u
spdk_vhost_kill
timing_exit clean_vhost

timing_exit vhost_boot
