#!/usr/bin/env bash
set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/bdev/nbd_common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
vm_no="0"

function err_clean() {
	trap - ERR
	print_backtrace
	set +e
	error "Error on $1 $2"
	vm_kill_all
	$rpc_py vhost_scsi_controller_remove_target naa.vhost_vm.$vm_no 0
	$rpc_py vhost_delete_controller naa.vhost_vm.$vm_no
	$rpc_py bdev_lvol_delete $lvb_u
	$rpc_py bdev_lvol_delete_lvstore -u $lvs_u
	vhost_kill 0
	exit 1
}

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
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

vhosttestinit

trap 'err_clean "${FUNCNAME}" "${LINENO}"' ERR
timing_enter start_vhost
vhost_run -n 0
timing_exit start_vhost

timing_enter create_lvol

nvme_bdev=$($rpc_py bdev_get_bdevs -b Nvme0n1)
nvme_bdev_bs=$(jq ".[] .block_size" <<< "$nvme_bdev")
nvme_bdev_name=$(jq ".[] .name" <<< "$nvme_bdev")
if [[ $nvme_bdev_bs != 512 ]]; then
	echo "ERROR: Your device $nvme_bdev_name block size is $nvme_bdev_bs, but should be 512 bytes."
	false
fi

lvb_size=20000 # MB
lvs_u=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs0)
lvb_u=$($rpc_py bdev_lvol_create -u $lvs_u lvb0 "$lvb_size")
timing_exit create_lvol

timing_enter convert_vm_image
modprobe nbd
trap 'nbd_stop_disks $(get_vhost_dir 0)/rpc.sock /dev/nbd0; err_clean "${FUNCNAME}" "${LINENO}"' ERR
nbd_start_disks "$(get_vhost_dir 0)/rpc.sock" $lvb_u /dev/nbd0
qemu-img convert $os_image -O raw /dev/nbd0
sync
nbd_stop_disks $(get_vhost_dir 0)/rpc.sock /dev/nbd0
sleep 1
timing_exit convert_vm_image

trap 'err_clean "${FUNCNAME}" "${LINENO}"' ERR
timing_enter create_vhost_controller
$rpc_py vhost_create_scsi_controller naa.vhost_vm.$vm_no
$rpc_py vhost_scsi_controller_add_target naa.vhost_vm.$vm_no 0 $lvb_u
timing_exit create_vhost_controller

timing_enter setup_vm
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --disks="vhost_vm" --spdk-boot="vhost_vm"
vm_run $vm_no
vm_wait_for_boot 300 $vm_no
timing_exit setup_vm

start_part_sector=0 drive_size=0
while IFS=":" read -r id start end _; do
	start=${start%s} end=${end%s}
	[[ -b $id ]] && drive_size=$start
	[[ $id =~ ^[0-9]+$ ]] && start_part_sector=$((end + 1))
done < <(vm_exec "$vm_no" "parted /dev/sda -ms unit s print")

# If we didn't get a start sector for the partition then probably something is amiss. Also,
# check if size of the drive matches size used for creating lvb and if not, fail.
((start_part_sector > 0)) && (((drive_size * 512) >> 20 == lvb_size))

timing_enter run_vm_cmd
vm_exec $vm_no "parted -s /dev/sda mkpart primary ${start_part_sector}s 100%; sleep 1; partprobe"
vm_exec $vm_no "mkfs.ext4 -F /dev/sda2; mkdir -p /mnt/sda2test; mount /dev/sda2 /mnt/sda2test;"
vm_exec $vm_no "fio --name=integrity --bsrange=4k-512k --iodepth=128 --numjobs=1 --direct=1 \
 --thread=1 --group_reporting=1 --rw=randrw --rwmixread=70 --filename=/mnt/sda2test/test_file \
 --verify=md5 --do_verify=1 --verify_backlog=1024 --fsync_on_close=1 --runtime=20 \
 --time_based=1 --size=1024m"
vm_exec $vm_no "umount /mnt/sda2test; rm -rf /mnt/sda2test"
alignment_offset=$(vm_exec $vm_no "cat /sys/block/sda/sda1/alignment_offset")
echo "alignment_offset: $alignment_offset"
timing_exit run_vm_cmd

vm_shutdown_all

timing_enter clean_vhost
$rpc_py vhost_scsi_controller_remove_target naa.vhost_vm.$vm_no 0
$rpc_py vhost_delete_controller naa.vhost_vm.$vm_no
$rpc_py bdev_lvol_delete $lvb_u
$rpc_py bdev_lvol_delete_lvstore -u $lvs_u
timing_exit clean_vhost

vhost_kill 0

vhosttestfini
