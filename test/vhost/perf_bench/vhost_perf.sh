#!/usr/bin/env bash
set -e

vm_count=1
vm_memory=2048
vm_image="/home/sys_sgsw/vhost_vm_image.qcow2"
max_disks=""
ctrl_type="spdk_vhost_scsi"
use_split=false
throttle=false

lvol_stores=()
lvol_bdevs=()
used_vms=""

fio_bin="--fio-bin=/home/sys_sgsw/fio_ubuntu"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                  Print help and exit"
	echo "    --fio-bin=PATH          Path to FIO binary on host.;"
	echo "                            Binary will be copied to VM, static compilation"
	echo "                            of binary is recommended."
	echo "    --fio-job=PATH          Fio config to use for test."
	echo "    --vm-count=INT          Total number of virtual machines to launch in this test;"
	echo "                            Each VM will get one bdev (lvol or split vbdev)"
	echo "                            to run FIO test."
	echo "                            Default: 1"
	echo "    --vm-memory=INT         Amount of RAM memory (in MB) to pass to a single VM."
	echo "                            Default: 2048 MB"
	echo "    --vm-image=PATH         OS image to use for running the VMs."
	echo "                            Default: /home/sys_sgsw/vhost_vm_image.qcow2"
	echo "    --max-disks=INT         Maximum number of NVMe drives to use in test."
	echo "                            Default: will use all available NVMes."
	echo "    --ctrl-type=TYPE        Controller type to use for test:"
	echo "                            spdk_vhost_scsi - use spdk vhost scsi"
	echo "                            spdk_vhost_blk - use spdk vhost block"
	echo "                            Default: spdk_vhost_scsi"
	echo "    --use-split             Use split vbdevs instead of Logical Volumes"
	echo "    --throttle=INT          I/Os throttle rate in IOPS for each device on the VMs."
	echo "    --custom-cpu-cfg=PATH   Custom CPU config for test."
	echo "                            Default: spdk/test/vhost/common/autotest.config"
	echo "-x                          set -x for script debug"
	exit 0
}

function cleanup_lvol_cfg()
{
	notice "Removing lvol bdevs"
	for lvol_bdev in "${lvol_bdevs[@]}"; do
		$rpc_py destroy_lvol_bdev $lvol_bdev
		notice "lvol bdev $lvol_bdev removed"
	done

	notice "Removing lvol stores"
	for lvol_store in "${lvol_stores[@]}"; do
		$rpc_py destroy_lvol_store -u $lvol_store
		notice "lvol store $lvol_store removed"
	done
}

function cleanup_split_cfg()
{
	notice "Removing split vbdevs"
	for (( i=0; i<$max_disks; i++ ));do
		$rpc_py destruct_split_vbdev Nvme${i}n1
	done
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
			fio-job=*) fio_job="${OPTARG#*=}" ;;
			vm-count=*) vm_count="${OPTARG#*=}" ;;
			vm-memory=*) vm_memory="${OPTARG#*=}" ;;
			vm-image=*) vm_image="${OPTARG#*=}" ;;
			max-disks=*) max_disks="${OPTARG#*=}" ;;
			ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
			use-split) use_split=true ;;
			throttle) throttle=true ;;
			custom-cpu-cfg=*) custom_cpu_cfg="${OPTARG#*=}" ;;
			thin-provisioning) thin=" -t " ;;
			multi-os) multi_os=true ;;
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
. $(readlink -e "$(dirname $0)/../../../scripts/common.sh") || exit 1
COMMON_DIR="$(cd $(readlink -f $(dirname $0))/../common && pwd)"
rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

if [[ -n $custom_cpu_cfg ]]; then
	source $custom_cpu_cfg
fi

if [[ -z $fio_job ]]; then
	warning "No FIO job specified! Will use default from common directory."
	fio_job="$COMMON_DIR/fio_jobs/default_integrity.job"
fi

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
notice "Get NVMe disks:"
nvmes=($(iter_pci_class_code 01 08 02))

if [[ -z $max_disks ]]; then
	max_disks=${#nvmes[@]}
fi

if [[ ${#nvmes[@]} -lt max_disks ]]; then
	fail "Number of NVMe drives (${#nvmes[@]}) is lower than number of requested disks for test ($max_disks)"
fi

notice "running SPDK vhost"
spdk_vhost_run
notice "..."

# Calculate number of needed splits per NVMe
# so that each VM gets it's own bdev during test
splits=()

#Calculate least minimum number of splits on each disks
for i in `seq 0 $((max_disks - 1))`; do
	splits+=( $((vm_count / max_disks)) )
done

# Split up the remainder
for i in `seq 0 $((vm_count % max_disks - 1))`; do
	(( splits[i]++ ))
done

notice "Preparing NVMe setup..."
notice "Using $max_disks physical NVMe drives"
notice "Nvme split list: ${splits[@]}"
# Prepare NVMes - Lvols or Splits
if [[ $use_split == true ]]; then
	notice "Using split vbdevs"
	trap 'cleanup_split_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
	split_bdevs=()
	for (( i=0; i<$max_disks; i++ ));do
		out=$($rpc_py construct_split_vbdev Nvme${i}n1 ${splits[$i]})
		for s in $out; do
			split_bdevs+=("$s")
		done
	done
	bdevs=("${split_bdevs[@]}")
else
	notice "Using logical volumes"
	trap 'cleanup_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
	for (( i=0; i<$max_disks; i++ ));do
		ls_guid=$($rpc_py construct_lvol_store Nvme${i}n1 lvs_$i)
		lvol_stores+=("$ls_guid")
		for (( j=0; j<${splits[$i]}; j++)); do
			free_mb=$(get_lvs_free_mb "$ls_guid")
			size=$((free_mb / (${splits[$i]}-j) ))
			lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$j $size)
			lvol_bdevs+=("$lb_name")
		done
	done
	bdevs=("${lvol_bdevs[@]}")
fi

# Prepare VMs and controllers
for (( i=0; i<$vm_count; i++)); do
	vm="vm_$i"

	setup_cmd="vm_setup --disk-type=$ctrl_type --force=$i"
	setup_cmd+=" --os=$vm_image"

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py construct_vhost_scsi_controller naa.0.$i
		$rpc_py add_vhost_scsi_lun naa.0.$i 0 ${bdevs[$i]}
		setup_cmd+=" --disks=0"
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		$rpc_py construct_vhost_blk_controller naa.$i.$i ${bdevs[$i]}
		setup_cmd+=" --disks=$i"
	fi
	$setup_cmd
	used_vms+=" $i"
done

# Start VMs
# Run VMs
vm_run $used_vms
vm_wait_for_boot 300 $used_vms

# Run FIO
fio_disks=""
for vm_num in $used_vms; do
	vm_dir=$VM_BASE_DIR/$vm_num
	host_name="VM-$vm_num"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $vm_num

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	fi

	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

# Run FIO traffic
run_fio $fio_bin --job-file="$fio_job" --out="$TEST_DIR/fio_results" --json $fio_disks

notice "Shutting down virtual machines..."
vm_shutdown_all

#notice "Shutting down SPDK vhost app..."
if [[ $use_split == true ]]; then
	cleanup_split_cfg
else
	cleanup_lvol_cfg
fi
spdk_vhost_kill
