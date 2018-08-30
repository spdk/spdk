#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)
. $BASE_DIR/common.sh

disk_names=$(get_disks $DRIVER)
disks_numa=$(get_numa_node $DRIVER "$disk_names")
cores=$(get_cores "$CPUS_ALLOWED")
no_cores=($cores)
no_cores=${#no_cores[@]}

if $PRECONDITIONING; then
	HUGEMEM=8192 $ROOTDIR/scripts/setup.sh
	cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
	preconditioning
	rm -f $BASE_DIR/config.fio
fi

#Kernel polling mode driver
if [ $DRIVER = "kernel-classic-polling" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=pvsync2 --hipri=100"
	for disk in $disk_names; do
		echo -1 > /sys/block/$disk/queue/io_poll_delay
	done
#Kernel polling mode driver
elif [ $DRIVER = "kernel-hybrid-polling" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=pvsync2 --hipri=100"
	for disk in $disk_names; do
		echo 0 > /sys/block/$disk/queue/io_poll_delay
	done
elif [ $DRIVER = "kernel-libaio" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=libaio"
fi

result_dir=perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${DRIVER}_${date}
mkdir -p $BASE_DIR/results/$result_dir
result_file=$BASE_DIR/results/$result_dir/perf_results_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${DRIVER}_${date}.csv
unset iops_disks bw mean_lat_disks_usec p99_lat_disks_usec p99_99_lat_disks_usec stdev_disks_usec
echo "run-time,ramp-time,fio-plugin,QD,block-size,num-cpu-cores,workload,workload-mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $DRIVER $IODEPTH $BLK_SIZE $no_cores $RW $MIX >> $result_file
echo "num_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec],avg_slat[usec],avg_clat[usec],bw[Kib/s]" >> $result_file
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
		create_fio_config $k $DRIVER "$disk_names" "$disks_numa" "$cores"
		desc="Running Test: Blocksize=${BLK_SIZE} Workload=$RW MIX=${MIX} qd=${IODEPTH} io_plugin/driver=$DRIVER"

		if [ $DRIVER = "nvme" ] || [ $DRIVER = "bdev" ]; then
			run_spdk_nvme_fio $DRIVER "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			"--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
			"--numjobs=$NUMJOBS" "--description=$desc" "-log_avg_msec=250"\
			"--write_lat_log=$BASE_DIR/results/$result_dir/perf_lat_$${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${DRIVER}_${date}_${k}disks_${j}"
		else
			run_nvme_fio $fio_ioengine_opt "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			"--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
			"--numjobs=$NUMJOBS" "--description=$desc" "-log_avg_msec=250"\
			"--write_lat_log=$BASE_DIR/results/$result_dir/perf_lat_${BLK_SIZE}BS_${IODEPTH}QD_${RW}_${MIX}MIX_${DRIVER}_${date}_${k}disks_${j}"
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
		cp $NVME_FIO_RESULTS $BASE_DIR/results/$result_dir/perf_results_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}.json
		cp $BASE_DIR/config.fio $BASE_DIR/results/$result_dir/config_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}.fio
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
	iops_disks[$k]=$((${iops_disks[$k]} / $REPEAT_NO))
	mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} / $REPEAT_NO))
	p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} / $REPEAT_NO))
	p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} / $REPEAT_NO))
	stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} / $REPEAT_NO))
	mean_slat_disks_usec[$k]=$((${mean_slat_disks_usec[$k]} / $REPEAT_NO))
	mean_clat_disks_usec[$k]=$((${mean_clat_disks_usec[$k]} / $REPEAT_NO))
	bw[$k]=$((${bw[$k]} / $REPEAT_NO))

	printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} ${mean_lat_disks_usec[$k]} ${p99_lat_disks_usec[$k]}\
	${p99_99_lat_disks_usec[$k]} ${stdev_disks_usec[$k]} ${mean_slat_disks_usec[$k]} ${mean_clat_disks_usec[$k]} ${bw[$k]} >> $result_file

	#if tested on only one numeber of disk
	if $ONEWORKLOAD; then
		break
	fi
done
rm -f $BASE_DIR/bdev.conf $BASE_DIR/config.fio
