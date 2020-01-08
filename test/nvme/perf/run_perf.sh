#!/usr/bin/env bash

# Automated script that runs NVMe PMD/BDEV performance test.
# This script should be run as root. Please run the scripts/setup.sh before running this script to bind disks to VFIO/UIO driver
# This script takes the following parameters:
# "--run-time" - the run time for the workload in seconds
# "--ramp-time" - Fio will run the specified workload for this amount of time before logging any performance numbers
# "--cpu-allowed" - A comma-separated list of CPU cores used to run the workload. - When the spdk fio plugin is chosen, NVMe devices will
# be aligned to specific core according to their NUMA node. The script will try to align each core with devices matching core's
# on the same NUMA node first but if there are no devices left in the same NUMA node as the CPU Core then it will use devices on the other NUMA node.
# It is important to choose cores that will ensure best NUMA node allocation. For example, on a system with 8 devices on NUMA node
# 0 and 8 devices on NUMA node 1, cores 0-27 on numa node 0 and 28-55 on numa node 1, if test uses 16 disk and four cores
# then "--cpu-allowed=1,2,28,29" results in a NUMA-balanced configuration with 4 devices on each CPU core.
# However, if the test will use 10 CPU cores, then best option would be "--cpu-allowed=1,2,3,4,28,29,30,31,32,33" because cores 1-4 will be aligned with
# 2 devices on numa0 per core, cores 28-29 will be aligned with 2 devices on numa1 per core and cores 30-33 with 1 device on numa1 per core.
# "--iodepth" - Number of I/Os to keep in flight per devices for SPDK fio_plugin and per job for kernel driver.
# "--driver" - "This parameter is used to set the ioengine and other fio parameters that determine how fio jobs issue I/O. SPDK supports two modes (nvme and bdev): to use the SPDK BDEV fio plugin set the value to bdev, set the value to nvme to use the SPDK NVME PMD.
# "There are 4 modes available for Linux Kernel driver: set the value to kernel-libaio to use the Linux asynchronous I/O engine,
# set the value to kernel-classic-polling to use the pvsynch2 ioengine in classic polling mode (100% load on the polling CPU core),
# set the value to kernel-hybrid-polling to use the pvsynch2 ioengine in hybrid polling mode where the polling thread sleeps for half the mean device execution time,
# set the value to kernel-io-uring to use io_uring engine.
# "--no-preconditioning" - skip preconditioning - Normally the script will precondition disks to put them in a steady state.
# However, preconditioning could be skipped, for example preconditiong has been already made and workload was 100% reads.
# "--disk-no" - use specified number of disks for test.
# "--repeat-no" Repeat each workolad specified number of times.
# "--numjobs" - Number of fio threads running the workload.
# "--no-io-scaling" - Set number of iodepth to be per job instead per device for SPDK fio_plugin.
# An Example Performance Test Run
# "./spdk/test/perf/run_perf.sh --run-time=600 --ramp-time=60 --cpu-allowed=28 --fio-bin=/usr/src/fio/fio\
#  --rwmixread=100 --iodepth=256 --fio-plugin=bdev --no-preconditioning --disk-no=6"
# This command will run test using fio plugin for 600 seconds, 60 sec of ram time, randrw job with
# 100% reads with io depth 256 per disk, on 6 devices and skips preconditioning. Cpu core used for this test is
# core no 28.
BASE_DIR=$(readlink -f $(dirname $0))
. $BASE_DIR/common.sh

trap 'rm -f *.state $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
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
	$ROOT_DIR/scripts/setup.sh reset
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
			echo 0 > $sysfs/io_poll_delay
		done
	fi
fi

result_dir=$BASE_DIR/results/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}
result_file=$result_dir/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}.csv
mkdir -p $result_dir
unset iops_disks bw mean_lat_disks_usec p99_lat_disks_usec p99_99_lat_disks_usec stdev_disks_usec
echo "run-time,ramp-time,fio-plugin,QD,block-size,num-cpu-cores,workload,workload-mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE $NO_CORES $RW $MIX >> $result_file
echo "num_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec],avg_slat[usec],avg_clat[usec],bw[Kib/s]" >> $result_file
#Run each workolad $REPEAT_NO times
for (( j=0; j < REPEAT_NO; j++ ))
do
	#Start with $DISKNO disks and remove 2 disks for each run to avoid preconditioning before each run.
	for (( k=DISKNO; k >= 1; k-=2 ))
	do
		cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
		echo "" >> $BASE_DIR/config.fio
		#The SPDK fio plugin supports submitting/completing I/Os to multiple SSDs from a single thread.
		#Therefore, the per thread queue depth is set to the desired IODEPTH/device X the number of devices per thread.
		if [[ "$PLUGIN" =~ "spdk-plugin" ]] && [[ "$NOIOSCALING" = false ]]; then
			qd=$(( IODEPTH * k ))
		else
			qd=$IODEPTH
		fi

		if [ $PLUGIN = "spdk-perf-bdev" ]; then
			run_bdevperf > $NVME_FIO_RESULTS
			iops_disks[$k]=$((${iops_disks[$k]} + $(get_bdevperf_results iops)))
			bw[$k]=$((${bw[$k]} + $(get_bdevperf_results bw_Kibs)))
			cp $NVME_FIO_RESULTS $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.output
		elif [ $PLUGIN = "spdk-perf-nvme" ]; then
			run_nvmeperf $k > $NVME_FIO_RESULTS
			read -r iops bandwidth mean_lat min_lat max_lat <<< $(get_nvmeperf_results)

			iops_disks[$k]=$((${iops_disks[$k]} + iops))
			bw[$k]=$((${bw[$k]} + bandwidth))
			mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} + mean_lat))
			min_lat_disks_usec[$k]=$((${min_lat_disks_usec[$k]} + min_lat))
			max_lat_disks_usec[$k]=$((${max_lat_disks_usec[$k]} + max_lat))

			cp $NVME_FIO_RESULTS $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.output
		else
			create_fio_config $k $PLUGIN "$DISK_NAMES" "$DISKS_NUMA" "$CORES"
			desc="Running Test: Blocksize=${BLK_SIZE} Workload=$RW MIX=${MIX} qd=${IODEPTH} io_plugin/driver=$PLUGIN"

			cat <<- EOF >> $BASE_DIR/config.fio
				rw=$RW
				rwmixread=$MIX
				iodepth=$qd
				bs=$BLK_SIZE
				runtime=$RUNTIME
				ramp_time=$RAMP_TIME
				numjobs=$NUMJOBS
				time_based=1
				description=$desc
				log_avg_msec=250
				EOF

			echo "USING CONFIG:"
			cat $BASE_DIR/config.fio

			if [[ "$PLUGIN" =~ "spdk-plugin" ]]; then
				run_spdk_nvme_fio $PLUGIN "--output=$NVME_FIO_RESULTS" \
				"--write_lat_log=$result_dir/perf_lat_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}_${k}disks_${j}"
			else
				run_nvme_fio $fio_ioengine_opt "--output=$NVME_FIO_RESULTS" \
				"--write_lat_log=$result_dir/perf_lat_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${DATE}_${k}disks_${j}"
			fi

			#Store values for every number of used disks
			iops_disks[$k]=$((${iops_disks[$k]} + $(get_results iops $MIX)))
			mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} + $(get_results mean_lat_usec $MIX)))
			p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} + $(get_results p99_lat_usec $MIX)))
			p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} + $(get_results p99_99_lat_usec $MIX)))
			stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} + $(get_results stdev_usec $MIX)))

			mean_slat_disks_usec[$k]=$((${mean_slat_disks_usec[$k]} + $(get_results mean_slat_usec $MIX)))
			mean_clat_disks_usec[$k]=$((${mean_clat_disks_usec[$k]} + $(get_results mean_clat_usec $MIX)))
			bw[$k]=$((${bw[$k]} + $(get_results bw_Kibs $MIX)))
			cp $NVME_FIO_RESULTS $result_dir/perf_results_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.json
			cp $BASE_DIR/config.fio $result_dir/config_${MIX}_${PLUGIN}_${NO_CORES}cpus_${DATE}_${k}_disks_${j}.fio
			rm -f $BASE_DIR/config.fio
		fi

		#if tested on only one number of disk
		if $ONEWORKLOAD; then
			break
		fi
	done
done
#Write results to csv file
for (( k=DISKNO; k >= 1; k-=2 ))
do
	iops_disks[$k]=$((${iops_disks[$k]} / REPEAT_NO))

	if [[ "$PLUGIN" =~ "plugin" ]]; then
		mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} / REPEAT_NO))
		p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} / REPEAT_NO))
		p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} / REPEAT_NO))
		stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} / REPEAT_NO))
		mean_slat_disks_usec[$k]=$((${mean_slat_disks_usec[$k]} / REPEAT_NO))
		mean_clat_disks_usec[$k]=$((${mean_clat_disks_usec[$k]} / REPEAT_NO))
	elif [[ "$PLUGIN" == "spdk-perf-bdev" ]]; then
		mean_lat_disks_usec[$k]=0
		p99_lat_disks_usec[$k]=0
		p99_99_lat_disks_usec[$k]=0
		stdev_disks_usec[$k]=0
		mean_slat_disks_usec[$k]=0
		mean_clat_disks_usec[$k]=0
	elif [[ "$PLUGIN" == "spdk-perf-nvme" ]]; then
		mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} / REPEAT_NO))
		p99_lat_disks_usec[$k]=0
		p99_99_lat_disks_usec[$k]=0
		stdev_disks_usec[$k]=0
		mean_slat_disks_usec[$k]=0
		mean_clat_disks_usec[$k]=0
	fi

	bw[$k]=$((${bw[$k]} / REPEAT_NO))

	printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} ${mean_lat_disks_usec[$k]} ${p99_lat_disks_usec[$k]}\
	${p99_99_lat_disks_usec[$k]} ${stdev_disks_usec[$k]} ${mean_slat_disks_usec[$k]} ${mean_clat_disks_usec[$k]} ${bw[$k]} >> $result_file

	#if tested on only one numeber of disk
	if $ONEWORKLOAD; then
		break
	fi
done

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
rm -f $BASE_DIR/bdev.conf $BASE_DIR/config.fio
