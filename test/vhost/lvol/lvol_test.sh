#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/scripts/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

vm_count=1
ctrl_type="spdk_vhost_scsi"
use_fs=false
distribute_cores=false

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --fio-bin=PATH        Path to FIO binary.;"
	echo "    --vm-count=INT        Virtual machines to use in test;"
	echo "                          Each VM will get one lvol bdev on each NVMe."
	echo "                          Default: 1"
	echo "    --ctrl-type=TYPE      Controller type to use for test:"
	echo "                          spdk_vhost_scsi - use spdk vhost scsi"
	echo "                          spdk_vhost_blk - use spdk vhost block"
	echo "    --thin-provisioning   Create lvol bdevs thin provisioned instead of"
	echo "                          allocating space up front"
	echo "    --distribute-cores    Use custom config file and run vhost controllers"
	echo "                          on different CPU cores instead of single core."
	echo "                          Default: False"
	echo "-x                        set -x for script debug"
	exit 0
}

function clean_lvol_cfg() {
	notice "Removing lvol bdevs"
	for lvol_bdev in "${lvol_bdevs[@]}"; do
		$rpc_py bdev_lvol_delete $lvol_bdev
		notice "lvol bdev $lvol_bdev removed"
	done

	notice "Removing lvol stores"
	$rpc_py bdev_lvol_delete_lvstore -u "$ls_guid"
	notice "lvol store $ls_guid removed"
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
				vm-count=*) vm_count="${OPTARG#*=}" ;;
				ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
				distribute-cores) distribute_cores=true ;;
				thin-provisioning) thin=" -t " ;;
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

spdk_mask=$vhost_0_reactor_mask
if $distribute_cores; then
	source $testdir/autotest.config
	# Adjust the mask so vhost runs on separate cpus than qemu instances.
	# We know that .config sets qemus to run on single cpu so simply take
	# the next cpu and add some extra.
	# FIXME: Rewrite this so the config is more aware of what cpu topology
	# is actually available on the host system.
	spdk_mask=$((1 << vm_count))
	((spdk_mask |= 1 << (vm_count + 1)))
	((spdk_mask |= 1 << (vm_count + 2)))
	((spdk_mask |= 1 << (vm_count + 3)))
	spdk_mask=$(printf '0x%x' "$spdk_mask")
fi

trap 'error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR

vm_kill_all

notice "running SPDK vhost"
vhost_run -n "0" -a "--cpumask $spdk_mask"
notice "..."

trap 'clean_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' SIGTERM SIGABRT ERR

lvol_bdevs=()
used_vms=""

id=0
# Create base lvol store on NVMe
notice "Creating lvol store on device Nvme${id}n1"
ls_guid=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs_$id -c 4194304)
# Create base lvol bdevs
for ((j = 0; j < vm_count; j++)); do
	notice "Creating lvol bdev for VM $id on lvol store $ls_guid"
	free_mb=$(get_lvs_free_mb "$ls_guid")
	size=$((free_mb / (vm_count - j)))
	lb_name=$($rpc_py bdev_lvol_create -u $ls_guid lbd_vm_$j $size $thin)
	lvol_bdevs+=("$lb_name")
done

bdev_info=$($rpc_py bdev_get_bdevs)
notice "Configuration after initial set-up:"
$rpc_py bdev_lvol_get_lvstores
echo "$bdev_info"

# Set up VMs
for ((i = 0; i < vm_count; i++)); do
	vm="vm_$i"

	# Get all lvol bdevs associated with this VM number
	bdevs=$(jq -r "map(select(.aliases[] | contains(\"$vm\")) | \
            .aliases[]) | join(\" \")" <<< "$bdev_info")
	bdevs=($bdevs)

	setup_cmd="vm_setup --disk-type=$ctrl_type --force=$i"
	setup_cmd+=" --os=$VM_IMAGE"

	# Create single SCSI controller or multiple BLK controllers for this VM
	mask_arg="--cpumask $spdk_mask"

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py vhost_create_scsi_controller naa.0.$i $mask_arg
		for ((j = 0; j < ${#bdevs[@]}; j++)); do
			$rpc_py vhost_scsi_controller_add_target naa.0.$i $j ${bdevs[$j]}
		done
		setup_cmd+=" --disks=0"
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		disk=""
		for ((j = 0; j < ${#bdevs[@]}; j++)); do
			$rpc_py vhost_create_blk_controller naa.$j.$i ${bdevs[$j]} $mask_arg
			disk+="${j}:"
		done
		disk="${disk::-1}"
		setup_cmd+=" --disks=$disk"
	fi

	$setup_cmd
	used_vms+=" $i"
done

$rpc_py vhost_get_controllers

# Run VMs
vm_run $used_vms
vm_wait_for_boot 300 $used_vms

# Get disk names from VMs and run FIO traffic

fio_disks=""
for vm_num in $used_vms; do
	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num-${!qemu_mask_param}"
	vm_exec $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $vm_num

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	fi

	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

if [[ $RUN_NIGHTLY -eq 1 ]]; then
	job_file="default_integrity_nightly.job"
else
	job_file="default_integrity.job"
fi
# Run FIO traffic
run_fio $fio_bin --job-file=$rootdir/test/vhost/common/fio_jobs/$job_file --out="$VHOST_DIR/fio_results" $fio_disks

notice "Shutting down virtual machines..."
vm_shutdown_all
sleep 2

notice "Cleaning up vhost - remove LUNs, controllers, lvol bdevs and lvol stores"
if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
	for ((i = 0; i < vm_count; i++)); do
		notice "Removing devices from vhost SCSI controller naa.0.$i"
		for ((j = 0; j < ${#bdevs[@]}; j++)); do
			$rpc_py vhost_scsi_controller_remove_target naa.0.$i $j
			notice "Removed device $j"
		done
		notice "Removing vhost SCSI controller naa.0.$i"
		$rpc_py vhost_delete_controller naa.0.$i
	done
elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
	for ((i = 0; i < vm_count; i++)); do
		for ((j = 0; j < ${#bdevs[@]}; j++)); do
			notice "Removing vhost BLK controller naa.$j.$i"
			$rpc_py vhost_delete_controller naa.$j.$i
			notice "Removed naa.$j.$i"
		done
	done
fi

clean_lvol_cfg

$rpc_py bdev_lvol_get_lvstores
$rpc_py bdev_get_bdevs
$rpc_py vhost_get_controllers

notice "Shutting down SPDK vhost app..."
vhost_kill 0

vhosttestfini
