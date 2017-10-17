#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

vms=()
vm_img=""
disk="Nvme0n1"
malloc_disk=false

#TODO: add more options, make debug option
function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for automated readonly test fo vhost-block"
	echo "For test details, check test_plan.md"
	echo
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --image               Patch to VM image"
	echo "    --disk                Disk name"
	echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
    echo "                          NUM - VM number (mandatory)"
    echo "                          OS - VM os disk path (optional)"
    echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
    echo "                          If test-type=spdk_vhost_blk then each disk can have additional size parameter, e.g."
    echo "                          --vm=X,os.qcow,DISK_size_35G; unit can be M or G; default - 20G"
	echo "-x                        set -x for script debug"
	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			image=*) vm_img="${OPTARG#*=}" ;;
			disk=*) disk="${OPTARG#*=}" ;;
			vm=*) vms+=("${OPTARG#*=}") ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) enable_script_debug=true ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

test_type=spdk_vhost_blk

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

if echo $disk | grep -qi $malloc; then
		malloc_disk=true
fi

. $BASE_DIR/../common/common.sh
source $BASE_DIR/common.sh

function blk_ro_tc1()
{
	print_tc_name ${FUNCNAME[0]}
	local vm_no="0"
	local disk_name=$disk
	local vhost_blk_name="naa.$disk_name.$vm_no"

	if malloc_disc=true; then
		disk_name=$($rpc_py construct_malloc_bdev 500 512)
		if [ $? != 0 ]; then
			error "Failed to create malloc bdev"
		fi
	fi

	$rpc_py construct_vhost_blk_controller $vhost_blk_name $disk_name

#TODO: reduce size of vhost block disk to 1GB
	setup_cmd="$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
	setup_cmd+=" -f $vm_no"
    setup_cmd+=" --os=$vm_img"
    setup_cmd+=" --disk=$disk_name"
	$setup_cmd

	echo "stating up the VM"
	$BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR $vm_no
	vm_wait_for_boot 600
	echo "executing script on the VM"
	vm_ssh $vm_no "bash -s" < $BASE_DIR/disabled_readonly_vm.sh
	vm_shutdown $vm_no
#TODO: vm needs to be shut down before removing controller. maybe kill it instead?
	sleep 7

	$rpc_py remove_vhost_controller $vhost_blk_name
	$rpc_py construct_vhost_blk_controller -r $vhost_blk_name $disk_name
	$setup_cmd

	echo "stating up the VM"
	$BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR $vm_no
	vm_wait_for_boot 600
	echo "executing script on the VM"
	vm_ssh $vm_no "bash -s" < $BASE_DIR/enabled_readonly_vm.sh
	vm_shutdown $vm_no
#TODO this sleep isn't necessary if there isn't a second test case. or maybe kill it instead?
	sleep 6
}

function blk_ro_tc2()
{
	print_tc_name ${FUNCNAME[0]}
}

spdk_vhost_run $BASE_DIR
blk_ro_tc1
#blk_ro_tc2
spdk_vhost_kill
