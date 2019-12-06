#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
PLUGIN_DIR_NVME=$ROOT_DIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOT_DIR/examples/bdev/fio_plugin
BDEVPERF_DIR=$ROOT_DIR/test/bdev/bdevperf
. $ROOT_DIR/scripts/common.sh || exit 1
. $ROOT_DIR/test/common/autotest_common.sh
NVME_FIO_RESULTS=$BASE_DIR/result.json

PRECONDITIONING=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=256
DISKNO=1
ONEWORKLOAD=false
CPUS_ALLOWED=1
NUMJOBS=1
REPEAT_NO=3
NOIOSCALING=false

function is_bdf_not_mounted() {
	local bdf=$1
	local blkname
	local mountpoints
	blkname=$(ls -l /sys/block/ | grep $bdf | awk '{print $9}')
	mountpoints=$(lsblk /dev/$blkname --output MOUNTPOINT -n | wc -w)
	return $mountpoints
}

function get_cores(){
	local cpu_list="$1"
	for cpu in ${cpu_list//,/ }; do
		echo $cpu
	done
}

function get_cores_numa_node(){
	local cores=$1
	for core in $cores; do
		lscpu -p=cpu,node | grep "^$core\b" | awk -F ',' '{print $2}'
	done
}

function get_numa_node(){
	local plugin=$1
	local disks=$2
	if [ "$plugin" = "nvme" ]; then
		for bdf in $disks; do
			local driver
			driver=$(grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}')
			# Use this check to ommit blacklisted devices ( not binded to driver with setup.sh script )
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				cat /sys/bus/pci/devices/$bdf/numa_node
			fi
		done
	elif [ "$plugin" = "bdev" ] || [ "$plugin" = "bdevperf" ]; then
		local bdevs
		bdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
		for name in $disks; do
			local bdev_bdf
			bdev_bdf=$(jq -r ".[] | select(.name==\"$name\").driver_specific.nvme.pci_address" <<< $bdevs)
			cat /sys/bus/pci/devices/$bdev_bdf/numa_node
		done
	else
		# Only target not mounted NVMes
		for bdf in $(iter_pci_class_code 01 08 02); do
			if is_bdf_not_mounted $bdf; then
				cat /sys/bus/pci/devices/$bdf/numa_node
			fi
		done
	fi
}

function get_disks(){
	local plugin=$1
	if [ "$plugin" = "nvme" ]; then
		for bdf in $(iter_pci_class_code 01 08 02); do
			driver=$(grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}')
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				echo "$bdf"
			fi
		done
	elif [ "$plugin" = "bdev" ] || [ "$plugin" = "bdevperf" ]; then
		local bdevs
		bdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
		jq -r '.[].name' <<< $bdevs
	else
		# Only target not mounted NVMes
		for bdf in $(iter_pci_class_code 01 08 02); do
			if is_bdf_not_mounted $bdf; then
				local blkname
				blkname=$(ls -l /sys/block/ | grep $bdf | awk '{print $9}')
				echo $blkname
			fi
		done
	fi
}

function get_disks_on_numa(){
	local devs=($1)
	local numas=($2)
	local numa_no=$3
	local disks_on_numa=""
	local i

	for (( i=0; i<${#devs[@]}; i++ ))
	do
		if [ ${numas[$i]} = $numa_no ]; then
			disks_on_numa=$((disks_on_numa+1))
		fi
	done
	echo $disks_on_numa
}

function create_fio_config(){
	local disk_no=$1
	local plugin=$2
	local disks=($3)
	local disks_numa=($4)
	local cores=($5)
	local total_disks=${#disks[@]}
	local no_cores=${#cores[@]}
	local filename=""

	local cores_numa
	cores_numa=($(get_cores_numa_node "$5"))
	local disks_per_core=$((disk_no/no_cores))
	local disks_per_core_mod=$((disk_no%no_cores))

	# For kernel dirver, each disk will be alligned with all cpus on the same NUMA node
	if [ "$plugin" != "nvme" ] && [ "$plugin" != "bdev" ]; then
		for (( i=0; i<disk_no; i++ ))
		do
			sed -i -e "\$a[filename${i}]" $BASE_DIR/config.fio
			filename="/dev/${disks[$i]}"
			sed -i -e "\$afilename=$filename" $BASE_DIR/config.fio
			cpu_used=""
				for (( j=0; j<no_cores; j++ ))
				do
					core_numa=${cores_numa[$j]}
					if [ "${disks_numa[$i]}" = "$core_numa" ]; then
						cpu_used+="${cores[$j]},"
					fi
				done
				sed -i -e "\$acpus_allowed=$cpu_used" $BASE_DIR/config.fio
				echo "" >> $BASE_DIR/config.fio
		done
	else
		for (( i=0; i<no_cores; i++ ))
		do
			core_numa=${cores_numa[$i]}
			total_disks_per_core=$disks_per_core
			if [ "$disks_per_core_mod" -gt "0" ]; then
				total_disks_per_core=$((disks_per_core+1))
				disks_per_core_mod=$((disks_per_core_mod-1))
			fi

			if [ "$total_disks_per_core" = "0" ]; then
				break
			fi

			sed -i -e "\$a[filename${i}]" $BASE_DIR/config.fio
			#use cpus_allowed as cpumask works only for cores 1-32
			sed -i -e "\$acpus_allowed=${cores[$i]}" $BASE_DIR/config.fio
			m=0	#counter of disks per cpu core numa
			n=0 #counter of all disks
			while [ "$m" -lt  "$total_disks_per_core" ]; do
				if [ ${disks_numa[$n]} = $core_numa ]; then
					m=$((m+1))
					if [ "$plugin" = "nvme" ]; then
						filename='trtype=PCIe traddr='${disks[$n]//:/.}' ns=1'
					elif [ "$plugin" = "bdev" ]; then
						filename=${disks[$n]}
					fi
					sed -i -e "\$afilename=$filename" $BASE_DIR/config.fio
					#Mark numa of n'th disk as "x" to mark it as claimed
					disks_numa[$n]="x"
				fi
				n=$((n+1))
				# If there is no more disks with numa node same as cpu numa node, switch to other numa node.
				if [ $n -ge $total_disks ]; then
					if [ "$core_numa" = "1" ]; then
						core_numa=0
					else
						core_numa=1
					fi
					n=0
				fi
			done
			echo "" >> $BASE_DIR/config.fio
		done
	fi
}

function preconditioning(){
	local dev_name=""
	local filename=""
	local i
	sed -i -e "\$a[preconditioning]" $BASE_DIR/config.fio

	# Generate filename argument for FIO.
	# We only want to target NVMes not bound to nvme driver.
	# If they're still bound to nvme that means they were skipped by
	# setup.sh on purpose.
	local nvme_list
	nvme_list=$(get_disks nvme)
	for nvme in $nvme_list; do
		dev_name='trtype=PCIe traddr='${nvme//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
	echo "** Preconditioning disks, this can take a while, depending on the size of disks."
	run_spdk_nvme_fio "nvme" --filename="$filename" --size=100% --loops=2 --bs=1M\
		--rw=write --iodepth=32
}

function get_results(){
	local reads_pct=$2
	local writes_pct=$((100-$2))

	case "$1" in
		iops)
		iops=$(cat $NVME_FIO_RESULTS | jq -r '.jobs[] | (.read.iops + .write.iops)')
		iops=${iops%.*}
		echo $iops
		;;
		mean_lat_usec)
		mean_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.lat_ns.mean * $reads_pct + .write.lat_ns.mean * $writes_pct)")
		mean_lat=${mean_lat%.*}
		echo $(( mean_lat/100000 ))
		;;
		p99_lat_usec)
		p99_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.000000\" * $reads_pct + .write.clat_ns.percentile.\"99.000000\" * $writes_pct)")
		p99_lat=${p99_lat%.*}
		echo $(( p99_lat/100000 ))
		;;
		p99_99_lat_usec)
		p99_99_lat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.990000\" * $reads_pct + .write.clat_ns.percentile.\"99.990000\" * $writes_pct)")
		p99_99_lat=${p99_99_lat%.*}
		echo $(( p99_99_lat/100000 ))
		;;
		stdev_usec)
		stdev=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.stddev * $reads_pct + .write.clat_ns.stddev * $writes_pct)")
		stdev=${stdev%.*}
		echo $(( stdev/100000 ))
		;;
		mean_slat_usec)
		mean_slat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.slat_ns.mean * $reads_pct + .write.slat_ns.mean * $writes_pct)")
		mean_slat=${mean_slat%.*}
		echo $(( mean_slat/100000 ))
		;;
		mean_clat_usec)
		mean_clat=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.clat_ns.mean * $reads_pct + .write.clat_ns.mean * $writes_pct)")
		mean_clat=${mean_clat%.*}
		echo $(( mean_clat/100000 ))
		;;
		bw_Kibs)
		bw=$(cat $NVME_FIO_RESULTS | jq -r ".jobs[] | (.read.bw + .write.bw)")
		bw=${bw%.*}
		echo $(( bw ))
		;;
	esac
}

function get_bdevperf_results(){
	case "$1" in
		iops)
		iops=$(grep Total $NVME_FIO_RESULTS | awk -F 'Total' '{print $2}' | awk '{print $2}')
		iops=${iops%.*}
		echo $iops
		;;
		bw_Kibs)
		bw_MBs=$(grep Total $NVME_FIO_RESULTS | awk -F 'Total' '{print $2}' | awk '{print $4}')
		bw_MBs=${bw_MBs%.*}
		echo $(( bw_MBs * 1024 ))
		;;
	esac
}

function run_spdk_nvme_fio(){
	local plugin=$1
	echo "** Running fio test, this can take a while, depending on the run-time and ramp-time setting."
	if [ "$plugin" = "nvme" ]; then
		LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk
	elif [ "$plugin" = "bdev" ]; then
		LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk_bdev --spdk_conf=$BASE_DIR/bdev.conf --spdk_mem=4096
	fi

	sleep 1
}

function run_nvme_fio(){
	echo "** Running fio test, this can take a while, depending on the run-time and ramp-time setting."
	$FIO_BIN $BASE_DIR/config.fio --output-format=json "$@"
	sleep 1
}

function run_bdevperf(){
	echo "** Running bdevperf test, this can take a while, depending on the run-time setting."
	$BDEVPERF_DIR/bdevperf -c $BASE_DIR/bdev.conf -q $IODEPTH -o $BLK_SIZE -w $RW -M $MIX -t $RUNTIME
	sleep 1
}

function wait_for_nvme_reload() {
	local nvmes=$1

	shopt -s extglob
	for disk in $nvmes; do
		cmd="ls /sys/block/$disk/queue/*@(iostats|rq_affinity|nomerges|io_poll_delay)*"
		until $cmd 2>/dev/null; do
			echo "Waiting for full nvme driver reload..."
			sleep 0.5
		done
	done
	shopt -q extglob
}

function usage()
{
	set +x
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Run NVMe PMD/BDEV performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --run-time=TIME[s]    Tell fio to run the workload for the specified period of time. [default=$RUNTIME]"
	echo "    --ramp-time=TIME[s]   Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fio-bin=PATH        Path to fio binary. [default=$FIO_BIN]"
	echo "    --driver=STR          Use 'bdev' or 'nvme' for spdk driver with fio_plugin,"
	echo "                          'kernel-libaio', 'kernel-classic-polling', 'kernel-hybrid-polling' or"
	echo "                          'kernel-io-uring' for kernel driver. [default=$PLUGIN]"
	echo "    --max-disk=INT,ALL    Number of disks to test on, this will run multiple workloads with increasing number of disk each run, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --disk-no=INT,ALL     Number of disks to test on, this will run one workload on selected number od disks, it discards max-disk setting, if =ALL then test on all found disk"
	echo "    --rw=STR              Type of I/O pattern. Accepted values are randrw,rw. [default=$RW]"
	echo "    --rwmixread=INT       Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT         Number of I/Os to keep in flight against the file. [default=$IODEPTH]"
	echo "    --cpu-allowed=INT     Comma-separated list of CPU cores used to run the workload. [default=$CPUS_ALLOWED]"
	echo "    --repeat-no=INT       How many times to repeat each workload. [default=$REPEAT_NO]"
	echo "    --block-size=INT      The  block  size  in  bytes  used for I/O units. [default=$BLK_SIZE]"
	echo "    --numjobs=INT         Create the specified number of clones of this job. [default=$NUMJOBS]"
	echo "    --no-preconditioning  Skip preconditioning"
	echo "    --no-io-scaling       Do not scale iodepth for each device in SPDK fio plugin. [default=$NOIOSCALING]"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			run-time=*) RUNTIME="${OPTARG#*=}" ;;
			ramp-time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fio-bin=*) FIO_BIN="${OPTARG#*=}" ;;
			max-disk=*) DISKNO="${OPTARG#*=}" ;;
			disk-no=*) DISKNO="${OPTARG#*=}"; ONEWORKLOAD=true ;;
			driver=*) PLUGIN="${OPTARG#*=}" ;;
			rw=*) RW="${OPTARG#*=}" ;;
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			block-size=*) BLK_SIZE="${OPTARG#*=}" ;;
			no-preconditioning) PRECONDITIONING=false ;;
			no-io-scaling) NOIOSCALING=true ;;
			cpu-allowed=*) CPUS_ALLOWED="${OPTARG#*=}" ;;
			numjobs=*) NUMJOBS="${OPTARG#*=}" ;;
			repeat-no=*) REPEAT_NO="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

trap 'rm -f *.state $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
mkdir -p $BASE_DIR/results
date="$(date +'%m_%d_%Y_%H%M%S')"
if [[ $PLUGIN == "bdev" ]] || [[ $PLUGIN == "bdevperf" ]]; then
	$ROOT_DIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

disks=($(get_disks $PLUGIN))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
	exit 1
fi
