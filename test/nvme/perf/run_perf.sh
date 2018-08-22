#!/usr/bin/env bash

#Automated script that helps running NVMe performance test.
#Please run script as root. Before running script, bind disk to vfio/uio driver with setup.sh script.Example use:
#"./spdk/test/perf/run_perf.sh --run-time=600 --ramp-time=60 --cpu-allowed=28 --fio-bin=/usr/src/fio/fio\
# --rwmixread=100 --iodepth=256 --fio-plugin=bdev --no-preconditioning --disk-no=6"
#This command will run test using fio plugin for 600 seconds, 60 sec of ram time, randrw job with
#100% reads with io depth 256 per disk, on 6 devices and skips preconditioning. Cpu core used for this test is
#core no 28.
#
#run-time - run job for specified time in sec
#ramp-time - run job for specified time in sec before gathering results, basically wait for device to be in steady state
#cpu-allowed - use specified cpu cores - when spdk fio plugin is chosen, NVMe devices will be aligned to specific core
#according to its NUMA node. The script will try to align each core with devices matching core's NUMA first and if
#the is no devices left with corresponding NUMA then it will use devices with other NUMA node. It is important to choose cores
#that will ensure best NUMA node allocation. For example: On System with 8 devices on NUMA node 0 and 8 devices on NUMA node 1,
#cores 0-27 on numa node 0 and 28-55 on numa node 1, if test is set to use 16 disk and four cores then "--cpu-allowed=1,2,28,29"
#can be used resulting with 4 devices with node0 per core 1 and 2 and 4 devices with node1 per core 28 and 29. If 10 cores are
#required then best option would be "--cpu-allowed=1,2,3,4,28,29,30,31,32,33" because cores 1-4 will be aligned with 2 devices
#on numa0 per core and cores 28-33 will be aligned with 1 device on numa1 per core.
#iodepth - How many ios FIO has to issue
#fio-plugin - SPDK fio plugin - bdev or nvme
#no-preconditioning - skip preconditioning - Normally would do preconditioning of the disks to set disk to known, steady state
#before running workload but it could be skipped, for example preconditiong has been already made and workload was 100% reads.
#disk-no - use specified number of disks for test.

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

. $BASE_DIR/common.sh

if [ $PLUGIN = "bdev" ]; then
	$ROOTDIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
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
#Run each workolad 3 times
for (( j=0; j < 3; j++ ))
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
