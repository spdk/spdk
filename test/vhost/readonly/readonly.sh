#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
rpc_py="$BASE_DIR/../../../scripts/rpc.py "
RPC_PORT=5260

. $BASE_DIR/../common/common.sh

vm_img=""
disk="Nvme0n1_size_1G"
malloc_disk=false
x=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for automated readonly test fo vhost-block"
	echo "For test details, check test_plan.md"
	echo
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --image=              Patch to VM image"
	echo "    --disk=               Disk name and size. Disk can heva additional size parameter,"
	echo "                          e.g. --disk=Nvme0n1_size_35G; unit can be M or G; default - 20G."
	echo "                          If disk=malloc, then creates malloc disk. For malloc disks, size is always 512M,"
	echo "                          e.g. --disk=malloc. (Default: Nvme0n1_size_1G)"
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
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

test_type=spdk_vhost_blk

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

if echo $disk | grep -qi "malloc"; then
		malloc_disk=true
fi

. $BASE_DIR/../common/common.sh

function print_tc_name()
{
	echo ""
	echo "==============================================================="
	echo "Now running: $1"
	echo "==============================================================="
}

function blk_ro_tc1()
{
	print_tc_name ${FUNCNAME[0]}
	local vm_no="0"
	local disk_name=$disk
	local vhost_blk_name=""

	if $malloc_disk; then
		disk_name=$($rpc_py construct_malloc_bdev 512 4096)
		if [ $? != 0 ]; then
			error "Failed to create malloc bdev"
		fi

		disk=$disk_name"_size_512M"
	else
		disk_name=${disk%%_*}
	fi

	vhost_blk_name="naa.$disk_name.$vm_no"
	$rpc_py construct_vhost_blk_controller $vhost_blk_name $disk_name
	setup_cmd="$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
	setup_cmd+=" -f $vm_no"
	setup_cmd+=" --os=$vm_img"
	setup_cmd+=" --disk=$disk"
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
if [[ -z $x ]]; then
	set +x
fi

blk_ro_tc1
#blk_ro_tc2
spdk_vhost_kill
