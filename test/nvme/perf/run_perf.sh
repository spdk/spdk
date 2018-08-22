#!/usr/bin/env bash

# Automated script that runs NVMe PMD/BDEV performance test.
# This script should be run as root. Please run the scripts/setup.sh before running this script to bind disks to VFIO/UIO driver
# This script takes the following parameters:
# "--run-time" - the run time for the workload in seconds
# "--ramp-time" - Fio will run the specified workload for this amount of time before logging any performance numbers
# "--cpu-allowed" - A comma-separated list of CPU cores used to run the workload. - when the spdk fio plugin is chosen, NVMe devices will
# be aligned to specific core according to their NUMA node. The script will try to align each core with devices matching core's
# NUMA first but if there are no devices left with corresponding NUMA then it will use devices on the NUMA node.
# It is important to choose cores that will ensure best NUMA node allocation. For example, on a system with 8 devices on NUMA node
# 0 and 8 devices on NUMA node 1, cores 0-27 on numa node 0 and 28-55 on numa node 1, if test uses 16 disk and four cores
# then "--cpu-allowed=1,2,28,29" results in a NUMA-balanced configuration with 4 devices on each CPU core.
# However, if the test will use 10 CPU cores, then best option would be "--cpu-allowed=1,2,3,4,28,29,30,31,32,33" because cores 1-4,28 will be aligned with
# 2 devices on numa0 per core, cores 28-29 will be aligned with 2 devices on numa1 per core and cores 30-33 with 1 device on numa1.
# "--iodepth" - Number of I/Os to keep in flight per fio job.
# "--fio-plugin" - The SPDK fio plugin used in this test - bdev or nvme
# "--no-preconditioning" - skip preconditioning - Normally the script will precondition disks to put them in a steady state.
# However, preconditioning could be skipped, for example preconditiong has been already made and workload was 100% reads.
# "--disk-no" - use specified number of disks for test.
# "--repeat-no" Run each workolad specified number of times.
# An Example Performance Test Run
# "./spdk/test/perf/run_perf.sh --run-time=600 --ramp-time=60 --cpu-allowed=28 --fio-bin=/usr/src/fio/fio\
#  --rwmixread=100 --iodepth=256 --fio-plugin=bdev --no-preconditioning --disk-no=6"
# This command will run test using fio plugin for 600 seconds, 60 sec of ram time, randrw job with
# 100% reads with io depth 256 per disk, on 6 devices and skips preconditioning. Cpu core used for this test is
# core no 28.
BASE_DIR=$(readlink -f $(dirname $0))
. $BASE_DIR/common.sh
if [ $PLUGIN = "bdev" ]; then
	$ROOT_DIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

disk_names=$(get_disks $PLUGIN)
disks_numa=$(get_numa_node $PLUGIN "$disk_names")
cores=$(get_cores "$CPUS_ALLOWED")
no_cores=($cores)
no_cores=${#no_cores[@]}

if $PRECONDITIONING; then
	cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
	preconditioning
	rm -f $BASE_DIR/config.fio
fi

result_dir=perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${date}
mkdir $BASE_DIR/results/$result_dir
result_file=$BASE_DIR/results/$result_dir/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${PLUGIN}_${date}.csv
unset iops_disks mean_lat_disks p99_lat_disks p99_99_lat_disks stdev_disks
echo "run-time,ramp-time,fio-plugin,QD,block-size,num-cpu-cores,workload,workload-mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE $no_cores $RW $MIX >> $result_file
echo "num_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec]" >> $result_file
#Run each workolad $REPEAT_NO times
for (( j=0; j < $REPEAT_NO; j++ ))
do
	#Start with $DISKNO disks and remove 2 disks for each run to avoid preconditioning before each run.
	for (( k=$DISKNO; k >= 1; k-=2 ))
	do
		cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
		echo "" >> $BASE_DIR/config.fio
		#The SPDK fio plugin supports submitting/completing I/Os to multiple SSDs from a single thread.
		#Therefore, the per thread queue depth is set to the desired IODEPTH/device X the number of devices per thread.
		qd=$(( $IODEPTH * $k ))
		filename=$(create_fio_config $k $PLUGIN "$disk_names" "$disks_numa" "$cores")
		desc="Running Test: Blocksize=${BLK_SIZE} Workload=$RW MIX=${MIX} qd=${IODEPTH} fio_plugin=$PLUGIN"
		run_spdk_nvme_fio $PLUGIN "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
		 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
		 "--description=$desc"

		#Store values for every number of used disks
		iops_disks[$k]=$((${iops_disks[$k]} + $(get_results iops $MIX)))
		mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} + $(get_results mean_lat_usec $MIX)))
		p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} + $(get_results p99_lat_usec $MIX)))
		p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} + $(get_results p99_99_lat_usec $MIX)))
		stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} + $(get_results stdev_usec $MIX)))
		cp $NVME_FIO_RESULTS $BASE_DIR/results/$result_dir/perf_results_${MIX}_${PLUGIN}_${no_cores}cpus_${date}_${k}_disks_${j}.json
		cp $BASE_DIR/config.fio $BASE_DIR/results/$result_dir/config_${MIX}_${PLUGIN}_${no_cores}cpus_${date}_${k}_disks_${j}.fio
		rm -f $BASE_DIR/config.fio

		#if tested on only one number of disk
		if $ONEWORKLOAD; then
			break
		fi
	done
done
#Write results to csv file
for (( k=$DISKNO; k >= 1; k-=2 ))
do
	iops_disks[$k]=$((${iops_disks[$k]} / 3))
	mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} / 3))
	p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} / 3))
	p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} / 3))
	stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} / 3))

	printf "%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} ${mean_lat_disks_usec[$k]} ${p99_lat_disks_usec[$k]}\
	 ${p99_99_lat_disks_usec[$k]} ${stdev_disks_usec[$k]} >> $result_file

	#if tested on only one numeber of disk
	if $ONEWORKLOAD; then
		break
	fi
done
rm -f $BASE_DIR/bdev.conf $BASE_DIR/config.fio
