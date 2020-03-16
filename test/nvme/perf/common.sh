#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
rootdir=$ROOT_DIR
PLUGIN_DIR_NVME=$ROOT_DIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOT_DIR/examples/bdev/fio_plugin
BDEVPERF_DIR=$ROOT_DIR/test/bdev/bdevperf
NVMEPERF_DIR=$ROOT_DIR/examples/nvme/perf
. $ROOT_DIR/scripts/common.sh || exit 1
. $ROOT_DIR/test/common/autotest_common.sh
NVME_FIO_RESULTS=$BASE_DIR/result.json

declare -A KERNEL_ENGINES
KERNEL_ENGINES=(
				["kernel-libaio"]="--ioengine=libaio"
				["kernel-classic-polling"]="--ioengine=pvsync2 --hipri=100"
				["kernel-hybrid-polling"]="--ioengine=pvsync2 --hipri=100"
				["kernel-io-uring"]="--ioengine=io_uring" )

RW=randrw
MIX=100
IODEPTH=256
BLK_SIZE=4096
RUNTIME=600
RAMP_TIME=30
NUMJOBS=1
REPEAT_NO=3
FIO_BIN=$CONFIG_FIO_SOURCE_DIR/fio
PLUGIN="nvme"
DISKNO=1
CPUS_ALLOWED=1
NOIOSCALING=false
PRECONDITIONING=true
ONEWORKLOAD=false
DATE="$(date +'%m_%d_%Y_%H%M%S')"

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
	if [[ "$plugin" =~ "nvme" ]]; then
		for bdf in $disks; do
			local driver
			driver=$(grep DRIVER /sys/bus/pci/devices/$bdf/uevent |awk -F"=" '{print $2}')
			# Use this check to ommit blacklisted devices ( not binded to driver with setup.sh script )
			if [ "$driver" = "vfio-pci" ] || [ "$driver" = "uio_pci_generic" ]; then
				cat /sys/bus/pci/devices/$bdf/numa_node
			fi
		done
	elif [[ "$plugin" =~ "bdev" ]]; then
		local bdevs
		bdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf --json)
		for name in $disks; do
			local bdev_bdf
			bdev_bdf=$(jq -r ".[] | select(.name==\"$name\").driver_specific.nvme.pci_address" <<< $bdevs)
			cat /sys/bus/pci/devices/$bdev_bdf/numa_node
		done
	else
		# Only target not mounted NVMes
		for bdf in $(get_nvme_bdfs); do
			if is_bdf_not_mounted $bdf; then
				cat /sys/bus/pci/devices/$bdf/numa_node
			fi
		done
	fi
}

function get_disks(){
	local plugin=$1
	if [[ "$plugin" =~ "nvme" ]]; then
		for bdf in $(get_nvme_bdfs); do
			echo "$bdf"
		done
	elif [[ "$plugin" =~ "bdev" ]]; then
		local bdevs
		bdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf --json)
		jq -r '.[].name' <<< $bdevs
	else
		# Only target not mounted NVMes
		for bdf in $(get_nvme_bdfs); do
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
	if [[ "$plugin" =~ "kernel" ]]; then
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
					if [[ "$plugin" = "spdk-plugin-nvme" ]]; then
						filename='trtype=PCIe traddr='${disks[$n]//:/.}' ns=1'
					elif [[ "$plugin" = "spdk-plugin-bdev" ]]; then
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

function preconditioning() {
	local dev_name=""
	local filename=""
	local nvme_list

	HUGEMEM=8192 $ROOT_DIR/scripts/setup.sh
	cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
	echo "[Preconditioning]" >> $BASE_DIR/config.fio

	# Generate filename argument for FIO.
	# We only want to target NVMes not bound to nvme driver.
	# If they're still bound to nvme that means they were skipped by
	# setup.sh on purpose.
	nvme_list=$(get_disks nvme)
	for nvme in $nvme_list; do
		dev_name='trtype=PCIe traddr='${nvme//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
	echo "** Preconditioning disks, this can take a while, depending on the size of disks."
	run_spdk_nvme_fio "spdk-plugin-nvme" --filename="$filename" --size=100% --loops=2 --bs=1M \
		--rw=write --iodepth=32 --output-format=normal
	rm -f $BASE_DIR/config.fio
}

function get_results(){
	local reads_pct=$2
	local writes_pct=$((100-$2))

	case "$1" in
		iops)
		iops=$(jq -r '.jobs[] | (.read.iops + .write.iops)' $NVME_FIO_RESULTS)
		iops=${iops%.*}
		echo $iops
		;;
		mean_lat_usec)
		mean_lat=$(jq -r ".jobs[] | (.read.lat_ns.mean * $reads_pct + .write.lat_ns.mean * $writes_pct)" $NVME_FIO_RESULTS)
		mean_lat=${mean_lat%.*}
		echo $(( mean_lat/100000 ))
		;;
		p99_lat_usec)
		p99_lat=$(jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.000000\" * $reads_pct + .write.clat_ns.percentile.\"99.000000\" * $writes_pct)" $NVME_FIO_RESULTS)
		p99_lat=${p99_lat%.*}
		echo $(( p99_lat/100000 ))
		;;
		p99_99_lat_usec)
		p99_99_lat=$(jq -r ".jobs[] | (.read.clat_ns.percentile.\"99.990000\" * $reads_pct + .write.clat_ns.percentile.\"99.990000\" * $writes_pct)" $NVME_FIO_RESULTS)
		p99_99_lat=${p99_99_lat%.*}
		echo $(( p99_99_lat/100000 ))
		;;
		stdev_usec)
		stdev=$(jq -r ".jobs[] | (.read.clat_ns.stddev * $reads_pct + .write.clat_ns.stddev * $writes_pct)" $NVME_FIO_RESULTS)
		stdev=${stdev%.*}
		echo $(( stdev/100000 ))
		;;
		mean_slat_usec)
		mean_slat=$(jq -r ".jobs[] | (.read.slat_ns.mean * $reads_pct + .write.slat_ns.mean * $writes_pct)" $NVME_FIO_RESULTS)
		mean_slat=${mean_slat%.*}
		echo $(( mean_slat/100000 ))
		;;
		mean_clat_usec)
		mean_clat=$(jq -r ".jobs[] | (.read.clat_ns.mean * $reads_pct + .write.clat_ns.mean * $writes_pct)" $NVME_FIO_RESULTS)
		mean_clat=${mean_clat%.*}
		echo $(( mean_clat/100000 ))
		;;
		bw_Kibs)
		bw=$(jq -r ".jobs[] | (.read.bw + .write.bw)" $NVME_FIO_RESULTS)
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

function get_nvmeperf_results() {
	local iops
	local bw_MBs
	local mean_lat_usec
	local max_lat_usec
	local min_lat_usec

	read -r iops bw_MBs mean_lat_usec min_lat_usec max_lat_usec<<< $(tr -s " " < $NVME_FIO_RESULTS | grep -oP "(?<=Total : )(.*+)")

	# We need to get rid of the decimal spaces due
	# to use of arithmetic expressions instead of "bc" for calculations
	iops=${iops%.*}
	bw_MBs=${bw_MBs%.*}
	mean_lat_usec=${mean_lat_usec%.*}
	min_lat_usec=${min_lat_usec%.*}
	max_lat_usec=${max_lat_usec%.*}

	echo "$iops $(bc <<< "$bw_MBs * 1024") $mean_lat_usec $min_lat_usec $max_lat_usec"
}

function run_spdk_nvme_fio(){
	local plugin=$1
	echo "** Running fio test, this can take a while, depending on the run-time and ramp-time setting."
	if [[ "$plugin" = "spdk-plugin-nvme" ]]; then
		LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk
	elif [[ "$plugin" = "spdk-plugin-bdev" ]]; then
		LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
		 "${@:2}" --ioengine=spdk_bdev --spdk_json_conf=$BASE_DIR/bdev.conf --spdk_mem=4096
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
	$BDEVPERF_DIR/bdevperf --json $BASE_DIR/bdev.conf -q $IODEPTH -o $BLK_SIZE -w $RW -M $MIX -t $RUNTIME -m "[$CPUS_ALLOWED]"
	sleep 1
}

function run_nvmeperf() {
	# Prepare -r argument string for nvme perf command
	local r_opt
	local disks

	# Limit the number of disks to $1 if needed
	disks=( $(get_disks nvme) )
	disks=( "${disks[@]:0:$1}" )
	r_opt=$(printf -- ' -r "trtype:PCIe traddr:%s"' "${disks[@]}")

	echo "** Running nvme perf test, this can take a while, depending on the run-time setting."

	# Run command in separate shell as this solves quoting issues related to r_opt var
	$SHELL -c "$NVMEPERF_DIR/perf $r_opt -q $IODEPTH -o $BLK_SIZE -w $RW -M $MIX -t $RUNTIME -c [$CPUS_ALLOWED]"
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

function verify_disk_number() {
	# Check if we have appropriate number of disks to carry out the test
	if [[ "$PLUGIN" =~ "bdev" ]]; then
		cat <<-JSON >"$BASE_DIR/bdev.conf"
			{"subsystems":[
			$("$ROOT_DIR/scripts/gen_nvme.sh" --json)
			]}
		JSON
	fi

	disks=($(get_disks $PLUGIN))
	if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
		DISKNO=${#disks[@]}
	elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
		echo "error: Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
		false
	fi
}

function usage()
{
	set +x
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Run NVMe PMD/BDEV performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo
	echo "Workload parameters:"
	echo "    --rw=STR              Type of I/O pattern. Accepted values are randrw,rw. [default=$RW]"
	echo "    --rwmixread=INT       Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT         Number of I/Os to keep in flight against the file. [default=$IODEPTH]"
	echo "    --block-size=INT      The  block  size  in  bytes  used for I/O units. [default=$BLK_SIZE]"
	echo "    --run-time=TIME[s]    Tell fio to run the workload for the specified period of time. [default=$RUNTIME]"
	echo "    --ramp-time=TIME[s]   Fio will run the specified workload for this amount of time before"
	echo "                          logging any performance numbers. [default=$RAMP_TIME]. Applicable only for fio-based tests."
	echo "    --numjobs=INT         Create the specified number of clones of this job. [default=$NUMJOBS]"
	echo "                          Applicable only for fio-based tests."
	echo "    --repeat-no=INT       How many times to repeat workload test. [default=$REPEAT_NO]"
	echo "                          Test result will be an average of repeated test runs."
	echo "    --fio-bin=PATH        Path to fio binary. [default=$FIO_BIN]"
	echo "                          Applicable only for fio-based tests."
	echo
	echo "Test setup parameters:"
	echo "    --driver=STR          Selects tool used for testing. Choices available:"
	echo "                             - spdk-perf-nvme (SPDK nvme perf)"
	echo "                             - spdk-perf-bdev (SPDK bdev perf)"
	echo "                             - spdk-plugin-nvme (SPDK nvme fio plugin)"
	echo "                             - spdk-plugin-bdev (SPDK bdev fio plugin)"
	echo "                             - kernel-classic-polling"
	echo "                             - kernel-hybrid-polling"
	echo "                             - kernel-libaio"
	echo "                             - kernel-io-uring"
	echo "    --disk-no=INT,ALL     Number of disks to test on, this will run one workload on selected number od disks,"
	echo "                          it discards max-disk setting, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --max-disk=INT,ALL    Number of disks to test on, this will run multiple workloads with increasing number of disk each run."
	echo "                          If =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpu-allowed=INT     Comma-separated list of CPU cores used to run the workload. [default=$CPUS_ALLOWED]"
	echo "    --no-preconditioning  Skip preconditioning"
	echo "    --no-io-scaling       Do not scale iodepth for each device in SPDK fio plugin. [default=$NOIOSCALING]"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			rw=*) RW="${OPTARG#*=}" ;;
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			block-size=*) BLK_SIZE="${OPTARG#*=}" ;;
			run-time=*) RUNTIME="${OPTARG#*=}" ;;
			ramp-time=*) RAMP_TIME="${OPTARG#*=}" ;;
			numjobs=*) NUMJOBS="${OPTARG#*=}" ;;
			repeat-no=*) REPEAT_NO="${OPTARG#*=}" ;;
			fio-bin=*) FIO_BIN="${OPTARG#*=}" ;;
			driver=*) PLUGIN="${OPTARG#*=}" ;;
			disk-no=*) DISKNO="${OPTARG#*=}"; ONEWORKLOAD=true ;;
			max-disk=*) DISKNO="${OPTARG#*=}" ;;
			cpu-allowed=*) CPUS_ALLOWED="${OPTARG#*=}" ;;
			no-preconditioning) PRECONDITIONING=false ;;
			no-io-scaling) NOIOSCALING=true ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done
