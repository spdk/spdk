#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

vhost_num="0"
vm_memory=2048
vm_sar_enable=false
host_sar_enable=false
sar_delay="0"
sar_interval="1"
sar_count="10"
vm_throttle=""
ctrl_type="spdk_vhost_scsi"
use_split=false
kernel_cpus=""
run_precondition=false
lvol_stores=()
lvol_bdevs=()
split_bdevs=()
used_vms=""
wwpn_prefix="naa.5001405bc6498"

fio_bin="--fio-bin=/home/sys_sgsw/fio_ubuntu"
fio_iterations=1
precond_fio_bin="/usr/src/fio/fio"
disk_map=""

disk_cfg_bdfs=()
disk_cfg_spdk_names=()
disk_cfg_splits=()
disk_cfg_vms=()
disk_cfg_kernel_names=()

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
	echo "    --disk-map              Disk map for given test. Specify which disks to use, their SPDK name,"
	echo "                            how many times to split them and which VMs should be attached to created bdevs."
	echo "                            Example:"
	echo "                            NVME PCI BDF,Spdk Bdev Name,Split Count,VM List"
	echo "                            0000:1a:00.0,Nvme0,2,0 1"
	echo "                            0000:1b:00.0,Nvme1,2,2 3"
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
	for disk in "${disk_cfg_spdk_names[@]}"; do
		$rpc_py bdev_split_delete ${disk}n1
	done
}

function cleanup_parted_config()
{
	notice "Removing parted disk configuration"
	for disk in "${disk_cfg_kernel_names[@]}"; do
		parted -s /dev/${disk}n1 rm 1
	done
}

function cleanup_kernel_vhost()
{
	notice "Cleaning kernel vhost configration"
	targetcli clearconfig confirm=True
	cleanup_parted_config
}

function create_vm() {
	vm_num=$1
	setup_cmd="vm_setup --disk-type=$ctrl_type --force=$vm_num --memory=$vm_memory --os=$VM_IMAGE"
	if [[ "$ctrl_type" == "kernel_vhost" ]]; then
		x=$(printf %03d $vm_num)
		setup_cmd+=" --disks=${wwpn_prefix}${x}"
	else
		setup_cmd+=" --disks=0"
	fi
	$setup_cmd
	used_vms+=" $vm_num"
	echo "Added to used vms"
	echo $used_vms
}

function create_spdk_controller() {
	vm_num=$1
	bdev=$2

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py vhost_create_scsi_controller naa.0.$vm_num
		notice "Created vhost scsi controller naa.0.$vm_num"
		$rpc_py vhost_scsi_controller_add_target naa.0.$vm_num 0 $bdev
		notice "Added LUN 0/$bdev to controller naa.0.$vm_num"
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		$rpc_py vhost_create_blk_controller naa.0.$vm_num $bdev
		notice "Created vhost blk controller naa.0.$vm_num $bdev"
	fi
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			fio-bin=*) fio_bin="--fio-bin=${OPTARG#*=}" ;;
			fio-job=*) fio_job="${OPTARG#*=}" ;;
			fio-iterations=*) fio_iterations="${OPTARG#*=}" ;;
			vm-memory=*) vm_memory="${OPTARG#*=}" ;;
			vm-image=*) VM_IMAGE="${OPTARG#*=}" ;;
			vm-sar-enable) vm_sar_enable=true ;;
			host-sar-enable) host_sar_enable=true ;;
			sar-delay=*) sar_delay="${OPTARG#*=}" ;;
			sar-interval=*) sar_interval="${OPTARG#*=}" ;;
			sar-count=*) sar_count="${OPTARG#*=}" ;;
			vm-throttle-iops=*) vm_throttle="${OPTARG#*=}" ;;
			ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
			use-split) use_split=true ;;
			run-precondition) run_precondition=true ;;
			precond-fio-bin=*) precond_fio_bin="${OPTARG#*=}" ;;
			limit-kernel-vhost=*) kernel_cpus="${OPTARG#*=}" ;;
			custom-cpu-cfg=*) custom_cpu_cfg="${OPTARG#*=}" ;;
			disk-map=*) disk_map="${OPTARG#*=}" ;;
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
	vhost_reactor_mask="vhost_${vhost_num}_reactor_mask"
	vhost_reactor_mask="${!vhost_reactor_mask}"
	vhost_master_core="vhost_${vhost_num}_master_core"
	vhost_master_core="${!vhost_master_core}"
fi

if [[ -z $fio_job ]]; then
	warning "No FIO job specified! Will use default from common directory."
	fio_job="$rootdir/test/vhost/common/fio_jobs/default_integrity.job"
fi

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR

if [[ -z $disk_map ]]; then
	fail "No disk map provided for test. Exiting."
fi

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

set +x
readarray disk_cfg < $disk_map
for line in "${disk_cfg[@]}"; do
	echo $line
	IFS=","
	s=($line)
	disk_cfg_bdfs+=(${s[0]})
	disk_cfg_spdk_names+=(${s[1]})
	disk_cfg_splits+=(${s[2]})
	disk_cfg_vms+=("${s[3]}")

	# Find kernel nvme names
	if [[ "$ctrl_type" == "kernel_vhost" ]]; then
		tmp=$(find /sys/devices/pci* -name ${s[0]} -print0 | xargs sh -c 'ls $0/nvme')
		disk_cfg_kernel_names+=($tmp)
		IFS=" "
	fi
done
unset IFS
set -x


if [[ "$ctrl_type" == "kernel_vhost" ]]; then
	notice "Configuring kernel vhost..."
	trap 'vm_kill_all; sleep 1; cleanup_kernel_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR

	# Split disks using parted for kernel vhost
	newline=$'\n'
	backstores=()
	for (( i=0; i<${#disk_cfg_kernel_names[@]}; i++ )); do
		nvme=${disk_cfg_kernel_names[$i]}
		splits=${disk_cfg_splits[$i]}
		notice "  Creating extended partition on disk /dev/${nvme}n1"
		parted -s /dev/${nvme}n1 mklabel msdos
		parted -s /dev/${nvme}n1 mkpart extended 2048s 100%

		part_size=$((100/${disk_cfg_splits[$i]})) # Split 100% of disk into roughly even parts
		echo "  Creating  ${splits} partitions of relative disk size ${part_size}"
		for p in $(seq 0 $((splits - 1))); do
			p_start=$((p*part_size))
			p_end=$((p_start+part_size))
			parted -s /dev/${nvme}n1 mkpart logical ${p_start}% ${p_end}%
			sleep 3
		done

		# Prepare kernel vhost configuration
		# Below grep: match only NVMe partitions which are not "Extended" type.
		# For example: will match nvme0n1p15 but not nvme0n1p1
		partitions=$(find /dev -name "${nvme}n1*" | sort --version-sort | grep -P 'p(?!1$)\d+')
		# Create block backstores for vhost kernel process
		for p in $partitions; do
			backstore_name=$(basename $p)
			backstores+=("$backstore_name")
			targetcli backstores/block create $backstore_name $p
		done
		partitions=($partitions)

		# Create kernel vhost controllers and add LUNs
		# Setup VM configurations
		vms_to_run=(${disk_cfg_vms[i]})
		for (( j=0; j<${#vms_to_run[@]}; j++ )); do
			# WWPN prefix misses 3 characters. Need to complete it
			# using block backstore number
			x=$(printf %03d ${vms_to_run[$j]})
			wwpn="${wwpn_prefix}${x}"
			targetcli vhost/ create $wwpn
			targetcli vhost/$wwpn/tpg1/luns create /backstores/block/$(basename ${partitions[$j]})
			create_vm ${vms_to_run[j]}
			sleep 1
		done
	done
	targetcli ls
else
	notice "Configuring SPDK vhost..."
	vhost_run "${vhost_num}" "--no-gen-nvme" "-p ${vhost_master_core}" "-m ${vhost_reactor_mask}"
	notice "..."

	if [[ $use_split == true ]]; then
		notice "Configuring split bdevs configuration..."
		trap 'cleanup_split_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
		for (( i=0; i<${#disk_cfg_bdfs[@]}; i++ )); do
			nvme_bdev=$($rpc_py bdev_nvme_attach_controller -b ${disk_cfg_spdk_names[$i]} -t pcie -a ${disk_cfg_bdfs[$i]})
			notice "Created NVMe Bdev: $nvme_bdev with BDF ${disk_cfg_bdfs[$i]}"

			splits=$($rpc_py bdev_split_create $nvme_bdev ${disk_cfg_splits[$i]})
			splits=($splits)
			notice "Created splits: ${splits[*]} on Bdev ${nvme_bdev}"
			for s in "${splits[@]}"; do
				split_bdevs+=($s)
			done

			vms_to_run=(${disk_cfg_vms[i]})
			for (( j=0; j<${#vms_to_run[@]}; j++ )); do
				notice "Setting up VM ${vms_to_run[j]}"
				create_spdk_controller "${vms_to_run[j]}" ${splits[j]}
				create_vm ${vms_to_run[j]}
			done
			echo " "
		done
		bdevs=("${split_bdevs[@]}")
	else
		notice "Configuring LVOLs..."
		trap 'cleanup_lvol_cfg; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR
		for (( i=0; i<${#disk_cfg_bdfs[@]}; i++ )); do
			nvme_bdev=$($rpc_py bdev_nvme_attach_controller -b ${disk_cfg_spdk_names[$i]} -t pcie -a ${disk_cfg_bdfs[$i]})
			notice "Created NVMe Bdev: $nvme_bdev with BDF ${disk_cfg_bdfs[$i]}"

			ls_guid=$($rpc_py bdev_lvol_create_lvstore $nvme_bdev lvs_$i --clear-method none)
			lvol_stores+=("$ls_guid")
			notice "Created Lvol Store: $ls_guid on Bdev $nvme_bdev"

			vms_to_run=(${disk_cfg_vms[i]})
			for (( j=0; j<${disk_cfg_splits[$i]}; j++)); do
				free_mb=$(get_lvs_free_mb "$ls_guid")
				size=$((free_mb / ((${disk_cfg_splits[$i]}-j)) ))
				lb_name=$($rpc_py bdev_lvol_create -u $ls_guid lbd_$j $size --clear-method none)
				lvol_bdevs+=("$lb_name")
				notice "Created LVOL Bdev $lb_name on Lvol Store $ls_guid on Bdev $nvme_bdev"

				notice "Setting up VM ${vms_to_run[j]}"
				create_spdk_controller "${vms_to_run[j]}" ${lb_name}
				create_vm ${vms_to_run[j]}
			done
			echo " "
		done
		$rpc_py bdev_lvol_get_lvstores
	fi
	$rpc_py bdev_get_bdevs
	$rpc_py vhost_get_controllers
fi

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
	vhost_kill "${vhost_num}"
fi

if [[ -n "$kernel_cpus" ]]; then
	rmdir /sys/fs/cgroup/cpuset/spdk
fi
