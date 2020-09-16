#!/usr/bin/env bash
set -e

# Dir variables and sourcing common files
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugin_dir=$rootdir/build/fio
bdevperf_dir=$rootdir/test/bdev/bdevperf
nvmeperf_dir=$rootdir/build/examples
source $testdir/common.sh
source $rootdir/scripts/common.sh || exit 1
source $rootdir/test/common/autotest_common.sh

# Global & default variables
declare -A KERNEL_ENGINES
KERNEL_ENGINES=(
	["kernel-libaio"]="--ioengine=libaio"
	["kernel-classic-polling"]="--ioengine=pvsync2 --hipri=100"
	["kernel-hybrid-polling"]="--ioengine=pvsync2 --hipri=100"
	["kernel-io-uring"]="--ioengine=io_uring")

RW=randrw
MIX=100
IODEPTH=256
BLK_SIZE=4096
RUNTIME=600
RAMP_TIME=30
NUMJOBS=1
REPEAT_NO=3
GTOD_REDUCE=false
SAMPLING_INT=0
IO_BATCH_SUBMIT=0
IO_BATCH_COMPLETE=0
FIO_BIN=$CONFIG_FIO_SOURCE_DIR/fio
TMP_RESULT_FILE=$testdir/result.json
PLUGIN="nvme"
DISKCFG=""
BDEV_CACHE=""
BDEV_POOL=""
DISKNO="ALL"
CPUS_ALLOWED=1
NOIOSCALING=false
PRECONDITIONING=true
CPUFREQ=""
PERFTOP=false
DPDKMEM=false
DATE="$(date +'%m_%d_%Y_%H%M%S')"

function usage() {
	set +x
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Run NVMe PMD/BDEV performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo
	echo "Workload parameters:"
	echo "    --rw=STR                 Type of I/O pattern. Accepted values are randrw,rw. [default=$RW]"
	echo "    --rwmixread=INT          Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT            Number of I/Os to keep in flight against the file. [default=$IODEPTH]"
	echo "    --block-size=INT         The  block  size  in  bytes  used for I/O units. [default=$BLK_SIZE]"
	echo "    --run-time=TIME[s]       Tell fio to run the workload for the specified period of time. [default=$RUNTIME]"
	echo "    --ramp-time=TIME[s]      Fio will run the specified workload for this amount of time before"
	echo "                             logging any performance numbers. [default=$RAMP_TIME]. Applicable only for fio-based tests."
	echo "    --numjobs=INT            Create the specified number of clones of this job. [default=$NUMJOBS]"
	echo "                             Applicable only for fio-based tests."
	echo "    --repeat-no=INT          How many times to repeat workload test. [default=$REPEAT_NO]"
	echo "                             Test result will be an average of repeated test runs."
	echo "    --gtod-reduce            Enable fio gtod_reduce option. [default=$GTOD_REDUCE]"
	echo "    --sampling-int=INT       Value for fio log_avg_msec parameters [default=$SAMPLING_INT]"
	echo "    --io-batch-submit=INT    Value for iodepth_batch_submit fio option [default=$IO_BATCH_SUBMIT]"
	echo "    --io-batch-complete=INT  Value for iodepth_batch_complete fio option [default=$IO_BATCH_COMPLETE]"
	echo "    --fio-bin=PATH           Path to fio binary. [default=$FIO_BIN]"
	echo "                             Applicable only for fio-based tests."
	echo
	echo "Test setup parameters:"
	echo "    --driver=STR            Selects tool used for testing. Choices available:"
	echo "                               - spdk-perf-nvme (SPDK nvme perf)"
	echo "                               - spdk-perf-bdev (SPDK bdev perf)"
	echo "                               - spdk-plugin-nvme (SPDK nvme fio plugin)"
	echo "                               - spdk-plugin-bdev (SPDK bdev fio plugin)"
	echo "                               - kernel-classic-polling"
	echo "                               - kernel-hybrid-polling"
	echo "                               - kernel-libaio"
	echo "                               - kernel-io-uring"
	echo "    --disk-config           Configuration file containing PCI BDF addresses of NVMe disks to use in test."
	echo "                            It consists a single column of PCI addresses. SPDK Bdev names will be assigned"
	echo "                            and Kernel block device names detected."
	echo "                            Lines starting with # are ignored as comments."
	echo "    --bdev-io-cache-size    Set IO cache size for for SPDK bdev subsystem."
	echo "    --bdev-io-pool-size     Set IO pool size for for SPDK bdev subsystem."
	echo "    --max-disk=INT,ALL      Number of disks to test on, this will run multiple workloads with increasing number of disk each run."
	echo "                            If =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpu-allowed=INT/PATH  Comma-separated list of CPU cores used to run the workload. Ranges allowed."
	echo "                            Can also point to a file containing list of CPUs. [default=$CPUS_ALLOWED]"
	echo "    --no-preconditioning    Skip preconditioning"
	echo "    --no-io-scaling         Do not scale iodepth for each device in SPDK fio plugin. [default=$NOIOSCALING]"
	echo "    --cpu-frequency=INT     Run tests with CPUs set to a desired frequency. 'intel_pstate=disable' must be set in"
	echo "                            GRUB options. You can use 'cpupower frequency-info' and 'cpupower frequency-set' to"
	echo "                            check list of available frequencies. Example: --cpu-frequency=1100000."
	echo
	echo "Other options:"
	echo "    --perftop           Run perftop measurements on the same CPU cores as specified in --cpu-allowed option."
	echo "    --dpdk-mem-stats    Dump DPDK memory stats during the test."
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help)
					usage $0
					exit 0
					;;
				rw=*) RW="${OPTARG#*=}" ;;
				rwmixread=*) MIX="${OPTARG#*=}" ;;
				iodepth=*) IODEPTH="${OPTARG#*=}" ;;
				block-size=*) BLK_SIZE="${OPTARG#*=}" ;;
				run-time=*) RUNTIME="${OPTARG#*=}" ;;
				ramp-time=*) RAMP_TIME="${OPTARG#*=}" ;;
				numjobs=*) NUMJOBS="${OPTARG#*=}" ;;
				repeat-no=*) REPEAT_NO="${OPTARG#*=}" ;;
				gtod-reduce) GTOD_REDUCE=true ;;
				sampling-int=*) SAMPLING_INT="${OPTARG#*=}" ;;
				io-batch-submit=*) IO_BATCH_SUBMIT="${OPTARG#*=}" ;;
				io-batch-complete=*) IO_BATCH_COMPLETE="${OPTARG#*=}" ;;
				fio-bin=*) FIO_BIN="${OPTARG#*=}" ;;
				driver=*) PLUGIN="${OPTARG#*=}" ;;
				disk-config=*)
					DISKCFG="${OPTARG#*=}"
					if [[ ! -f "$DISKCFG" ]]; then
						echo "Disk confiuration file $DISKCFG does not exist!"
						exit 1
					fi
					;;
				bdev-io-cache-size=*) BDEV_CACHE="${OPTARG#*=}" ;;
				bdev-io-pool-size=*) BDEV_POOL="${OPTARG#*=}" ;;
				max-disk=*) DISKNO="${OPTARG#*=}" ;;
				cpu-allowed=*)
					CPUS_ALLOWED="${OPTARG#*=}"
					if [[ -f "$CPUS_ALLOWED" ]]; then
						CPUS_ALLOWED=$(cat "$CPUS_ALLOWED")
					fi
					;;
				no-preconditioning) PRECONDITIONING=false ;;
				no-io-scaling) NOIOSCALING=true ;;
				cpu-frequency=*) CPUFREQ="${OPTARG#*=}" ;;
				perftop) PERFTOP=true ;;
				dpdk-mem-stats) DPDKMEM=true ;;
				*)
					usage $0 echo "Invalid argument '$OPTARG'"
					exit 1
					;;
			esac
			;;
		h)
			usage $0
			exit 0
			;;
		*)
			usage $0 "Invalid argument '$optchar'"
			exit 1
			;;
	esac
done

result_dir=$testdir/results/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}
result_file=$result_dir/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}.csv
mkdir -p $result_dir
unset iops_disks bw mean_lat_disks_usec p90_lat_disks_usec p99_lat_disks_usec p99_99_lat_disks_usec stdev_disks_usec
echo "run-time,ramp-time,fio-plugin,QD,block-size,num-cpu-cores,workload,workload-mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE $NO_CORES $RW $MIX >> $result_file
echo "num_of_disks,iops,avg_lat[usec],p90[usec],p99[usec],p99.99[usec],stdev[usec],avg_slat[usec],avg_clat[usec],bw[Kib/s]" >> $result_file

trap 'rm -f *.state $testdir/bdev.conf; kill $perf_pid; wait $dpdk_mem_pid; print_backtrace' ERR SIGTERM SIGABRT

if [[ "$PLUGIN" =~ "bdev" ]]; then
	create_spdk_bdev_conf "$BDEV_CACHE" "$BDEV_POOL"
	echo "INFO: Generated bdev.conf file:"
	cat $testdir/bdev.conf
fi
verify_disk_number
DISK_NAMES=$(get_disks $PLUGIN)
DISKS_NUMA=$(get_numa_node $PLUGIN "$DISK_NAMES")
CORES=$(get_cores "$CPUS_ALLOWED")
NO_CORES_ARRAY=($CORES)
NO_CORES=${#NO_CORES_ARRAY[@]}

if $PRECONDITIONING; then
	preconditioning
fi

if [[ "$PLUGIN" =~ "kernel" ]]; then
	$rootdir/scripts/setup.sh reset
	fio_ioengine_opt="${KERNEL_ENGINES[$PLUGIN]}"

	if [[ $PLUGIN = "kernel-classic-polling" ]]; then
		for disk in $DISK_NAMES; do
			echo -1 > /sys/block/$disk/queue/io_poll_delay
		done
	elif [[ $PLUGIN = "kernel-hybrid-polling" ]]; then
		for disk in $DISK_NAMES; do
			echo 0 > /sys/block/$disk/queue/io_poll_delay
		done
	elif [[ $PLUGIN = "kernel-io-uring" ]]; then
		modprobe -rv nvme
		modprobe nvme poll_queues=8
		wait_for_nvme_reload $DISK_NAMES

		backup_dir="/tmp/nvme_param_bak"
		mkdir -p $backup_dir

		for disk in $DISK_NAMES; do
			echo "INFO: Backing up device parameters for $disk"
			sysfs=/sys/block/$disk/queue
			mkdir -p $backup_dir/$disk
			cat $sysfs/iostats > $backup_dir/$disk/iostats
			cat $sysfs/rq_affinity > $backup_dir/$disk/rq_affinity
			cat $sysfs/nomerges > $backup_dir/$disk/nomerges
			cat $sysfs/io_poll_delay > $backup_dir/$disk/io_poll_delay
		done

		for disk in $DISK_NAMES; do
			echo "INFO: Setting device parameters for $disk"
			sysfs=/sys/block/$disk/queue
			echo 0 > $sysfs/iostats
			echo 0 > $sysfs/rq_affinity
			echo 2 > $sysfs/nomerges
			echo -1 > $sysfs/io_poll_delay
		done
	fi
fi

if [[ -n "$CPUFREQ" ]]; then
	if [[ ! "$(cat /proc/cmdline)" =~ "intel_pstate=disable" ]]; then
		echo "ERROR: Cannot set custom CPU frequency for test. intel_pstate=disable not in boot options."
		false
	else
		cpu_governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
		cpupower frequency-set -g userspace
		cpupower frequency-set -f $CPUFREQ
	fi
fi

if $PERFTOP; then
	echo "INFO: starting perf record on cores $CPUS_ALLOWED"
	perf record -C $CPUS_ALLOWED -o "$testdir/perf.data" &
	perf_pid=$!
fi

if $DPDKMEM; then
	echo "INFO: waiting to generate DPDK memory usage"
	wait_time=$((RUNTIME / 2))
	if [[ ! "$PLUGIN" =~ "perf" ]]; then
		wait_time=$((wait_time + RAMP_TIME))
	fi
	(
		sleep $wait_time
		echo "INFO: generating DPDK memory usage"
		$rootdir/scripts/rpc.py env_dpdk_get_mem_stats
	) &
	dpdk_mem_pid=$!
fi

iops_disks=0
bw=0
min_lat_disks_usec=0
max_lat_disks_usec=0
mean_lat_disks_usec=0
p90_lat_disks_usec=0
p99_lat_disks_usec=0
p99_99_lat_disks_usec=0
stdev_disks_usec=0
mean_slat_disks_usec=0
mean_clat_disks_usec=0
#Run each workolad $REPEAT_NO times
for ((j = 0; j < REPEAT_NO; j++)); do
	if [ $PLUGIN = "spdk-perf-bdev" ]; then
		run_bdevperf > $TMP_RESULT_FILE
		read -r iops bandwidth <<< $(get_bdevperf_results)
		iops_disks=$(bc "$iops_disks + $iops")
		bw=$(bc "$bw + $bandwidth")
		cp $TMP_RESULT_FILE $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.output
	elif [ $PLUGIN = "spdk-perf-nvme" ]; then
		run_nvmeperf $DISKNO > $TMP_RESULT_FILE
		read -r iops bandwidth mean_lat min_lat max_lat <<< $(get_nvmeperf_results)

		iops_disks=$(bc "$iops_disks+$iops")
		bw=$(bc "$bw+$bandwidth")
		mean_lat_disks_usec=$(bc "$mean_lat_disks_usec + $mean_lat")
		min_lat_disks_usec=$(bc "$min_lat_disks_usec + $min_lat")
		max_lat_disks_usec=$(bc "$max_lat_disks_usec + $max_lat")

		cp $TMP_RESULT_FILE $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.output
	else
		create_fio_config $DISKNO $PLUGIN "$DISK_NAMES" "$DISKS_NUMA" "$CORES"

		if [[ "$PLUGIN" =~ "spdk-plugin" ]]; then
			run_spdk_nvme_fio $PLUGIN "--output=$TMP_RESULT_FILE" \
				"--write_lat_log=$result_dir/perf_lat_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}_${k}disks_${j}"
		else
			run_nvme_fio $fio_ioengine_opt "--output=$TMP_RESULT_FILE" \
				"--write_lat_log=$result_dir/perf_lat_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}_${k}disks_${j}"
		fi

		#Store values for every number of used disks
		#Use recalculated value for mixread param in case rw mode is not rw.
		rwmixread=$MIX
		if [[ $RW = *"read"* ]]; then
			rwmixread=100
		elif [[ $RW = *"write"* ]]; then
			rwmixread=0
		fi

		read -r iops bandwidth mean_lat_usec p90_lat_usec p99_lat_usec p99_99_lat_usec \
			stdev_usec mean_slat_usec mean_clat_usec <<< $(get_results $rwmixread)
		iops_disks=$(bc "$iops_disks + $iops")
		mean_lat_disks_usec=$(bc "$mean_lat_disks_usec + $mean_lat_usec")
		p90_lat_disks_usec=$(bc "$p90_lat_disks_usec + $p90_lat_usec")
		p99_lat_disks_usec=$(bc "$p99_lat_disks_usec + $p99_lat_usec")
		p99_99_lat_disks_usec=$(bc "$p99_99_lat_disks_usec + $p99_99_lat_usec")
		stdev_disks_usec=$(bc "$stdev_disks_usec + $stdev_usec")
		mean_slat_disks_usec=$(bc "$mean_slat_disks_usec + $mean_slat_usec")
		mean_clat_disks_usec=$(bc "$mean_clat_disks_usec + $mean_clat_usec")
		bw=$(bc "$bw + $bandwidth")

		cp $TMP_RESULT_FILE $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.json
		cp $testdir/config.fio $result_dir/config_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.fio
		rm -f $testdir/config.fio
	fi
done

if $PERFTOP; then
	echo "INFO: Stopping perftop measurements."
	kill $perf_pid
	wait $perf_pid || true
	perf report -i "$testdir/perf.data" > $result_dir/perftop_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}.txt
	rm -f "$testdir/perf.data"
fi

if $DPDKMEM; then
	mv "/tmp/spdk_mem_dump.txt" $result_dir/spdk_mem_dump_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}.txt
	echo "INFO: DPDK memory usage saved in $result_dir"
fi

#Write results to csv file
iops_disks=$(bc "$iops_disks / $REPEAT_NO")
bw=$(bc "$bw / $REPEAT_NO")
if [[ "$PLUGIN" =~ "plugin" ]] || [[ "$PLUGIN" =~ "kernel" ]]; then
	mean_lat_disks_usec=$(bc "$mean_lat_disks_usec / $REPEAT_NO")
	p90_lat_disks_usec=$(bc "$p90_lat_disks_usec / $REPEAT_NO")
	p99_lat_disks_usec=$(bc "$p99_lat_disks_usec / $REPEAT_NO")
	p99_99_lat_disks_usec=$(bc "$p99_99_lat_disks_usec / $REPEAT_NO")
	stdev_disks_usec=$(bc "$stdev_disks_usec / $REPEAT_NO")
	mean_slat_disks_usec=$(bc "$mean_slat_disks_usec / $REPEAT_NO")
	mean_clat_disks_usec=$(bc "$mean_clat_disks_usec / $REPEAT_NO")
elif [[ "$PLUGIN" == "spdk-perf-nvme" ]]; then
	mean_lat_disks_usec=$(bc "$mean_lat_disks_usec/$REPEAT_NO")
fi

printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" ${DISKNO} ${iops_disks} ${mean_lat_disks_usec} ${p90_lat_disks_usec} ${p99_lat_disks_usec} \
	${p99_99_lat_disks_usec} ${stdev_disks_usec} ${mean_slat_disks_usec} ${mean_clat_disks_usec} ${bw} >> $result_file

if [[ -n "$CPUFREQ" ]]; then
	cpupower frequency-set -g $cpu_governor
fi

if [ $PLUGIN = "kernel-io-uring" ]; then
	# Reload the nvme driver so that other test runs are not affected
	modprobe -rv nvme
	modprobe nvme
	wait_for_nvme_reload $DISK_NAMES

	for disk in $DISK_NAMES; do
		echo "INFO: Restoring device parameters for $disk"
		sysfs=/sys/block/$disk/queue
		cat $backup_dir/$disk/iostats > $sysfs/iostats
		cat $backup_dir/$disk/rq_affinity > $sysfs/rq_affinity
		cat $backup_dir/$disk/nomerges > $sysfs/nomerges
		cat $backup_dir/$disk/io_poll_delay > $sysfs/io_poll_delay
	done
fi
rm -f $testdir/bdev.conf $testdir/config.fio
