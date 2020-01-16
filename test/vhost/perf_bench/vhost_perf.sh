#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

vm_count=1
vm_memory=2048
vm_sar_enable=false
host_sar_enable=false
sar_delay="0"
sar_interval="1"
sar_count="10"
vm_throttle=""
max_disks=""
ctrl_type="spdk_vhost_scsi"
use_split=false
kernel_cpus=""
run_precondition=false
lvol_stores=()
lvol_bdevs=()
used_vms=""
wwpn_prefix="naa.5001405bc6498"

fio_bin="--fio-bin=/home/sys_sgsw/fio_ubuntu"
fio_iterations=1
precond_fio_bin="/usr/src/fio/fio"

function usage()
{
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                  Print help and exit"
	echo "    --fio-bin=PATH          Path to FIO binary on host.;"
	echo "                            Binary will be copied to VM, static compilation"
	echo "                            of binary is recommended."
	echo "    --fio-job=PATH          Fio config to use for test."
	echo "    --fio-iterations=INT    Number of times to run specified workload."
	echo "    --vm-count=INT          Total number of virtual machines to launch in this test;"
	echo "                            Each VM will get one bdev (lvol or split vbdev)"
	echo "                            to run FIO test."
	echo "                            Default: 1"
	echo "    --vm-memory=INT         Amount of RAM memory (in MB) to pass to a single VM."
	echo "                            Default: 2048 MB"
	echo "    --vm-image=PATH         OS image to use for running the VMs."
	echo "                            Default: \$HOME/vhost_vm_image.qcow2"
	echo "    --vm-sar-enable         Measure CPU utilization in guest VMs using sar."
	echo "    --host-sar-enable       Measure CPU utilization on host using sar."
	echo "    --sar-delay=INT         Wait for X seconds before starting SAR measurement. Default: 0."
	echo "    --sar-interval=INT      Interval (seconds) argument for SAR. Default: 1s."
	echo "    --sar-count=INT         Count argument for SAR. Default: 10."
	echo "    --vm-throttle-iops=INT  I/Os throttle rate in IOPS for each device on the VMs."
	echo "    --max-disks=INT         Maximum number of NVMe drives to use in test."
	echo "                            Default: will use all available NVMes."
	echo "    --ctrl-type=TYPE        Controller type to use for test:"
	echo "                            spdk_vhost_scsi - use spdk vhost scsi"
	echo "                            spdk_vhost_blk - use spdk vhost block"
	echo "                            kernel_vhost - use kernel vhost scsi"
	echo "                            Default: spdk_vhost_scsi"
	echo "    --use-split             Use split vbdevs instead of Logical Volumes"
	echo "    --limit-kernel-vhost=INT  Limit kernel vhost to run only on a number of CPU cores."
	echo "    --run-precondition      Precondition lvols after creating. Default: true."
	echo "    --precond-fio-bin       FIO binary used for SPDK fio plugin precondition. Default: /usr/src/fio/fio."
	echo "    --custom-cpu-cfg=PATH   Custom CPU config for test."
	echo "                            Default: spdk/test/vhost/common/autotest.config"
	echo "-x                          set -x for script debug"
	exit 0
}

function cleanup_lvol_cfg()
{
	notice "Removing lvol bdevs"
	for lvol_bdev in "${lvol_bdevs[@]}"; do
		$rpc_py bdev_lvol_delete $lvol_bdev
		notice "lvol bdev $lvol_bdev removed"
	done

	notice "Removing lvol stores"
	for lvol_store in "${lvol_stores[@]}"; do
		$rpc_py bdev_lvol_delete_lvstore -u $lvol_store
		notice "lvol store $lvol_store removed"
	done
}

function cleanup_split_cfg()
{
	notice "Removing split vbdevs"
	for (( i=0; i<max_disks; i++ ));do
		$rpc_py bdev_split_delete Nvme${i}n1
	done
}

function cleanup_parted_config()
{
	local disks
	disks=$(find /dev/ -maxdepth 1 -name 'nvme*n1' | sort --version-sort)
	for disk in $disks; do
		parted -s $disk rm 1
	done
}

function cleanup_kernel_vhost()
{
	notice "Cleaning kernel vhost configration"
	targetcli clearconfig confirm=True
	cleanup_parted_config
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
			fio-job=*) fio_job="${OPTARG#*=}" ;;
			fio-iterations=*) fio_iterations="${OPTARG#*=}" ;;
			vm-count=*) vm_count="${OPTARG#*=}" ;;
			vm-memory=*) vm_memory="${OPTARG#*=}" ;;
			vm-image=*) VM_IMAGE="${OPTARG#*=}" ;;
			vm-sar-enable) vm_sar_enable=true ;;
			host-sar-enable) host_sar_enable=true ;;
			sar-delay=*) sar_delay="${OPTARG#*=}" ;;
			sar-interval=*) sar_interval="${OPTARG#*=}" ;;
			sar-count=*) sar_count="${OPTARG#*=}" ;;
			vm-throttle-iops=*) vm_throttle="${OPTARG#*=}" ;;
			max-disks=*) max_disks="${OPTARG#*=}" ;;
			ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
			use-split) use_split=true ;;
			run-precondition) run_precondition=true ;;
			precond-fio-bin=*) precond_fio_bin="${OPTARG#*=}" ;;
			limit-kernel-vhost=*) kernel_cpus="${OPTARG#*=}" ;;
			custom-cpu-cfg=*) custom_cpu_cfg="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

if [[ -n $custom_cpu_cfg ]]; then
	source $custom_cpu_cfg
fi

if [[ -z $fio_job ]]; then
	warning "No FIO job specified! Will use default from common directory."
	fio_job="$rootdir/test/vhost/common/fio_jobs/default_integrity.job"
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


# Calculate number of needed splits per NVMe
# so that each VM gets it's own bdev during test.
splits=()

if [[ $vm_count -le $max_disks ]]; then
	for i in $(seq 0 $((max_disks - 1))); do
		splits+=("1")
	done
else
	#Calculate least minimum number of splits on each disks
	for i in $(seq 0 $((max_disks - 1))); do
		splits+=( $((vm_count / max_disks)) )
	done
	# Split up the remainder
	for i in $(seq 0 $((vm_count % max_disks - 1))); do
		(( splits[i]++ ))
	done
fi
notice "Preparing NVMe setup..."
notice "Using $max_disks physical NVMe drives"
notice "Nvme split list: ${splits[*]}"

# ===== Precondition NVMes if specified =====
if [[ $run_precondition == true ]]; then
	# Using the same precondition routine possible for lvols thanks
	# to --clear-method option. Lvols should not UNMAP on creation.
    $rootdir/scripts/gen_nvme.sh > $rootdir/nvme.cfg
    mapfile -t nvmes < <(grep -oP "Nvme\d+" $rootdir/nvme.cfg)
    fio_filename=$(printf ":%sn1" "${nvmes[@]}")
    fio_filename=${fio_filename:1}
    $precond_fio_bin --name="precondition" \
    --ioengine="${rootdir}/examples/bdev/fio_plugin/fio_plugin" \
    --rw="write" --spdk_conf="${rootdir}/nvme.cfg" --thread="1" \
    --group_reporting --direct="1" --size="100%" --loops="2" --bs="256k" \
    --iodepth=32 --filename="${fio_filename}" || true
fi

# ===== Prepare NVMe splits & run vhost process =====
if [[ "$ctrl_type" == "kernel_vhost" ]]; then
	trap 'vm_kill_all; sleep 1; cleanup_kernel_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
	# Split disks using parted for kernel vhost
	newline=$'\n'
	for (( i=0; i<max_disks; i++ ));do
		parted -s /dev/nvme${i}n1 mklabel msdos
		parted -s /dev/nvme${i}n1 mkpart extended 2048s 100%
		part_size=$((100/${splits[$i]})) # Split 100% of disk into roughly even parts
		echo "  Creating ${splits[$i]} partitions of relative disk size ${part_size}"

		for p in $(seq 0 $((${splits[$i]} - 1))); do
			p_start=$((p*part_size))
			p_end=$((p_start+part_size))
			parted -s /dev/nvme${i}n1 mkpart logical ${p_start}% ${p_end}%
		done
	done
	sleep 1

	# Prepare kernel vhost configuration
	# Below grep: match only NVMe partitions which are not "Extended" type.
	# For example: will match nvme0n1p15 but not nvme0n1p1
	partitions=$(find /dev/ -maxdepth 1 -name 'nvme*' | sort --version-sort | grep -P 'p(?!1$)\d+')
	backstores=()

	# Create block backstores for vhost kernel process
	for p in $partitions; do
		backstore_name=$(basename $p)
		backstores+=("$backstore_name")
		targetcli backstores/block create $backstore_name $p
	done

	# Create kernel vhost controllers and add LUNs
	for ((i=0; i<${#backstores[*]}; i++)); do
		# WWPN prefix misses 3 characters. Need to complete it
		# using block backstore number
		x=$(printf %03d $i)
		wwpn="${wwpn_prefix}${x}"
		targetcli vhost/ create $wwpn
		targetcli vhost/$wwpn/tpg1/luns create /backstores/block/${backstores[$i]}
	done
else
	# Run vhost process and prepare split vbdevs or lvol bdevs
	notice "running SPDK vhost"
	vhost_run 0
	notice "..."

	if [[ $use_split == true ]]; then
		notice "Using split vbdevs"
		trap 'cleanup_split_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
		split_bdevs=()
		for (( i=0; i<max_disks; i++ ));do
			out=$($rpc_py bdev_split_create Nvme${i}n1 ${splits[$i]})
			for s in $(seq 0 $((${splits[$i]}-1))); do
				split_bdevs+=("Nvme${i}n1p${s}")
			done
		done
		bdevs=("${split_bdevs[@]}")
	else
		notice "Using logical volumes"
		trap 'cleanup_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
		for (( i=0; i<max_disks; i++ ));do
			ls_guid=$($rpc_py bdev_lvol_create_lvstore Nvme${i}n1 lvs_$i --clear-method none)
			lvol_stores+=("$ls_guid")
			for (( j=0; j<${splits[$i]}; j++)); do
				free_mb=$(get_lvs_free_mb "$ls_guid")
				size=$((free_mb / (${splits[$i]}-j) ))
				lb_name=$($rpc_py bdev_lvol_create -u $ls_guid lbd_$j $size --clear-method none)
				lvol_bdevs+=("$lb_name")
			done
		done
		bdevs=("${lvol_bdevs[@]}")
	fi
fi

# Prepare VMs and controllers
for (( i=0; i<vm_count; i++)); do
	vm="vm_$i"

	setup_cmd="vm_setup --disk-type=$ctrl_type --force=$i --memory=$vm_memory"
	setup_cmd+=" --os=$VM_IMAGE"

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py vhost_create_scsi_controller naa.0.$i
		$rpc_py vhost_scsi_controller_add_target naa.0.$i 0 ${bdevs[$i]}
		setup_cmd+=" --disks=0"
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		$rpc_py vhost_create_blk_controller naa.$i.$i ${bdevs[$i]}
		setup_cmd+=" --disks=$i"
	elif [[ "$ctrl_type" == "kernel_vhost" ]]; then
		x=$(printf %03d $i)
		setup_cmd+=" --disks=${wwpn_prefix}${x}"
	fi
	$setup_cmd
	used_vms+=" $i"
done

# Start VMs
# Run VMs
vm_run $used_vms
vm_wait_for_boot 300 $used_vms

if [[ -n "$kernel_cpus" ]]; then
	mkdir -p /sys/fs/cgroup/cpuset/spdk
	kernel_mask=$vhost_0_reactor_mask
	kernel_mask=${kernel_mask#"["}
	kernel_mask=${kernel_mask%"]"}

	echo "$kernel_mask" >> /sys/fs/cgroup/cpuset/spdk/cpuset.cpus
	echo "0-1" >> /sys/fs/cgroup/cpuset/spdk/cpuset.mems

	kernel_vhost_pids=$(pgrep "vhost" -U root)
	for kpid in $kernel_vhost_pids; do
		echo "Limiting kernel vhost pid ${kpid}"
		echo "${kpid}" >> /sys/fs/cgroup/cpuset/spdk/tasks
	done
fi

# Run FIO
fio_disks=""
for vm_num in $used_vms; do
	host_name="VM-$vm_num"
	vm_exec $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $vm_num

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	elif [[ "$ctrl_type" == "kernel_vhost" ]]; then
		vm_check_scsi_location $vm_num
	fi

	if [[ -n "$vm_throttle" ]]; then
		block=$(printf '%s' $SCSI_DISK)
		major_minor=$(vm_exec "$vm_num" "cat /sys/block/$block/dev")
		vm_exec "$vm_num" "echo \"$major_minor $vm_throttle\" > /sys/fs/cgroup/blkio/blkio.throttle.read_iops_device"
		vm_exec "$vm_num" "echo \"$major_minor $vm_throttle\" > /sys/fs/cgroup/blkio/blkio.throttle.write_iops_device"
	fi

	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

# Run FIO traffic
fio_job_fname=$(basename $fio_job)
fio_log_fname="${fio_job_fname%%.*}.log"
for i in $(seq 1 $fio_iterations); do
	echo "Running FIO iteration $i"
	run_fio $fio_bin --job-file="$fio_job" --out="$VHOST_DIR/fio_results" --json $fio_disks &
	fio_pid=$!

	if $host_sar_enable || $vm_sar_enable; then
		pids=""
		mkdir -p $VHOST_DIR/fio_results/sar_stats
		sleep $sar_delay
	fi

	if $host_sar_enable; then
		sar -P ALL $sar_interval $sar_count > "$VHOST_DIR/fio_results/sar_stats/sar_stats_host.txt" &
		pids+=" $!"
	fi

	if $vm_sar_enable; then
		for vm_num in $used_vms; do
			vm_exec "$vm_num" "mkdir -p /root/sar; sar -P ALL $sar_interval $sar_count >> /root/sar/sar_stats_VM${vm_num}_run${i}.txt" &
			pids+=" $!"
		done
	fi

	for j in $pids; do
			wait $j
	done

	if $vm_sar_enable; then
		for vm_num in $used_vms; do
			vm_scp "$vm_num" "root@127.0.0.1:/root/sar/sar_stats_VM${vm_num}_run${i}.txt" "$VHOST_DIR/fio_results/sar_stats"
		done
	fi


	wait $fio_pid
	mv $VHOST_DIR/fio_results/$fio_log_fname $VHOST_DIR/fio_results/$fio_log_fname.$i
	sleep 1
done

notice "Shutting down virtual machines..."
vm_shutdown_all

if [[ "$ctrl_type" == "kernel_vhost" ]]; then
	cleanup_kernel_vhost || true
else
	notice "Shutting down SPDK vhost app..."
	if [[ $use_split == true ]]; then
		cleanup_split_cfg
	else
		cleanup_lvol_cfg
	fi
	vhost_kill 0
fi

if [[ -n "$kernel_cpus" ]]; then
	rmdir /sys/fs/cgroup/cpuset/spdk
fi
