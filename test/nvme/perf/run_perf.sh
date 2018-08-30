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

if [ $DRIVER = "kernel-classic" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=pvsync2 --hipri=100"
	for disk in $disk_names; do
		echo -1 > /sys/block/$disk/queue/io_poll_delay
	done
elif [ $DRIVER = "kernel-hybrid" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=pvsync2 --hipri=100"
	for disk in $disk_names; do
		echo 0 > /sys/block/$disk/queue/io_poll_delay
	done
elif [ $DRIVER = "kernel-default" ]; then
	$ROOTDIR/scripts/setup.sh reset
	fio_ioengine_opt="--ioengine=libaio"
fi

result_file=$BASE_DIR/results/perf_results_${MIX}_${DRIVER}_${no_cores}cpus_${date}.csv
unset iops_disks mean_lat_disks_usec p99_lat_disks_usec p99_99_lat_disks_usec stdev_disks_usec
echo "run-time,ramp-time,fio-plugin,QD,BS,no-cores,rw,mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $DRIVER $IODEPTH $BLK_SIZE $no_cores $RW $MIX >> $result_file
echo "no_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec],avg_slat[usec],avg_clat[usec]" >> $result_file
#Each Workload repeat $REPEAT_NO times
for (( j=0; j < $REPEAT_NO; j++ ))
do
	#Start with $DISKNO disks and remove 2 disks for each run to avoid preconditioning between each runs
	for (( k=$DISKNO; k >= 1; k-=2 ))
	do
		cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
		echo "" >> $BASE_DIR/config.fio
		#Set io depth to $IODEPTH per device
		qd=$(( $IODEPTH * $k ))
		create_fio_config $k $DRIVER "$disk_names" "$disks_numa" "$cores"
		desc="${BLK_SIZE}BS ${MIX}% MIX $RW IOPS test with $DRIVER fio_plugin/driver"

		if [ $DRIVER = "nvme" ] || [ $DRIVER = "bdev" ]; then
			run_spdk_nvme_fio $DRIVER "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			"--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
			"--numjobs=$NUMJOBS" "--description=$desc" "-log_avg_msec=250"\
			"--write_lat_log=$BASE_DIR/results/perf_lat_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}"
		else
			run_nvme_fio $fio_ioengine_opt "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			"--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
			"--numjobs=$NUMJOBS" "--description=$desc" "-log_avg_msec=250"\
			"--write_lat_log=$BASE_DIR/results/perf_lat_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}"
		fi

		#Store values for every number of used disks
		iops_disks[$k]=$((${iops_disks[$k]} + $(get_results iops $MIX)))
		mean_lat_disks_usec[$k]=$((${mean_lat_disks_usec[$k]} + $(get_results mean_lat_usec $MIX)))
		p99_lat_disks_usec[$k]=$((${p99_lat_disks_usec[$k]} + $(get_results p99_lat_usec $MIX)))
		p99_99_lat_disks_usec[$k]=$((${p99_99_lat_disks_usec[$k]} + $(get_results p99_99_lat_usec $MIX)))
		stdev_disks_usec[$k]=$((${stdev_disks_usec[$k]} + $(get_results stdev_usec $MIX)))
		mean_slat_disks_usec[$k]=$((${mean_slat_disks_usec[$k]} + $(get_results mean_slat_usec $MIX)))
		mean_clat_disks_usec[$k]=$((${mean_clat_disks_usec[$k]} + $(get_results mean_clat_usec $MIX)))
		cp $NVME_FIO_RESULTS $BASE_DIR/results/perf_results_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}.json
		cp $BASE_DIR/config.fio $BASE_DIR/results/config_${MIX}_${DRIVER}_${no_cores}cpus_${date}_${k}_disks_${j}.fio
		rm -f $BASE_DIR/config.fio

		#if tested on only one numeber of disk
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

	printf "%s,%s,%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} ${mean_lat_disks_usec[$k]} ${p99_lat_disks_usec[$k]}\
	 ${p99_99_lat_disks_usec[$k]} ${stdev_disks_usec[$k]} ${mean_slat_disks_usec[$k]} ${mean_clat_disks_usec[$k]} >> $result_file

	#if tested on only one numeber of disk
	if $ONEWORKLOAD; then
		break
	fi
done
rm -f $BASE_DIR/bdev.conf $BASE_DIR/config.fio
