#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
source $COMMON_DIR/common.sh

rpc_py="$BASE_DIR/../../../scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

vm_img=""
disk="Nvme0n1"
x=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for automated readonly test for vhost-block"
	echo "For test details check test_plan.md"
	echo
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --vm_image=           Path to VM image"
	echo "    --disk=               Disk name."
	echo "                          If disk=malloc, then creates malloc disk. For malloc disks, size is always 512M,"
	echo "                          e.g. --disk=malloc. (Default: Nvme0n1)"
	echo "-x                        set -x for script debug"
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
	-)
		case "$OPTARG" in
			help) usage $0 && exit 0;;
			vm_image=*) vm_img="${OPTARG#*=}" ;;
			disk=*) disk="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" && exit 1
		esac
		;;
	h)	usage $0 && exit 0 ;;
	x)	set -x
		x="-x" ;;
	*)	usage $0 "Invalid argument '$OPTARG'" && exit 1
	esac
done

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

if [[ $EUID -ne 0 ]]; then
	fail "Go away user come back as root"
fi

function print_tc_name()
{
	notice ""
	notice "==============================================================="
	notice "Now running: $1"
	notice "==============================================================="
}

function blk_ro_tc1()
{
	print_tc_name ${FUNCNAME[0]}
	local vm_no="0"
	local disk_name=$disk
	local vhost_blk_name=""
	local vm_dir="$TEST_DIR/vms/$vm_no"

	if [[ $disk =~ .*malloc.* ]]; then
		disk_name=$($rpc_py construct_malloc_bdev 512 4096)
		if [ $? != 0 ]; then
			fail "Failed to create malloc bdev"
		fi

		disk=$disk_name
	else
		disk_name=${disk%%_*}
		if ! $rpc_py get_bdevs | jq -r '.[] .name' | grep -qi $disk_name$; then
			fail "$disk_name bdev not found!"
		fi
	fi

#Create controller and create file on disk for later test
	notice "Creating vhost_blk controller"
	vhost_blk_name="naa.$disk_name.$vm_no"
	$rpc_py construct_vhost_blk_controller $vhost_blk_name $disk_name
	vm_setup --disk-type=spdk_vhost_blk --force=$vm_no --os=$vm_img --disks=$disk --read-only=true

	vm_run $vm_no
	vm_wait_for_boot 600 $vm_no
	notice "Preparing partition and file on guest VM"
	vm_ssh $vm_no "bash -s" < $BASE_DIR/disabled_readonly_vm.sh
	sleep 1

	vm_shutdown_all
#Create readonly controller and test readonly feature
	notice "Removing controller and creating new one with readonly flag"
	$rpc_py remove_vhost_controller $vhost_blk_name
	$rpc_py construct_vhost_blk_controller -r $vhost_blk_name $disk_name

	vm_run $vm_no
	vm_wait_for_boot 600 $vm_no
	notice "Testing readonly feature on guest VM"
	vm_ssh $vm_no "bash -s" < $BASE_DIR/enabled_readonly_vm.sh
	sleep 3

	vm_shutdown_all
#Delete file from disk and delete partition
	echo "INFO: Removing controller and creating new one"
	$rpc_py remove_vhost_controller $vhost_blk_name
	$rpc_py construct_vhost_blk_controller $vhost_blk_name $disk_name

	vm_run $vm_no
	vm_wait_for_boot 600 $vm_no
	notice "Removing partition and file from test disk on guest VM"
	vm_ssh $vm_no "bash -s" < $BASE_DIR/delete_partition_vm.sh
	sleep 1

	vm_shutdown_all
}

spdk_vhost_run --conf-path=$BASE_DIR
if [[ -z $x ]]; then
	set +x
fi

blk_ro_tc1
spdk_vhost_kill
