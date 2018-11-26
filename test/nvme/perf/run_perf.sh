#!/usr/bin/env bash

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

result_file=$BASE_DIR/results/perf_results_${MIX}_${PLUGIN}_${no_cores}cpus_${date}.csv
unset iops_disks mean_lat_disks p99_lat_disks p99_99_lat_disks stdev_disks
echo "run-time,ramp-time,fio-plugin,QD,BS,no-cores,rw,mix" > $result_file
printf "%s,%s,%s,%s,%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE $no_cores $RW $MIX >> $result_file
echo "no_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec]" >> $result_file
#Each Workload repeat 3 times
for (( j=0; j < 3; j++ ))
do
	#Start with $DISKNO disks and remove 2 disks for each run to avoid preconditioning between each runs
	for (( k=$DISKNO; k >= 1; k-=2 ))
	do
		cp $BASE_DIR/config.fio.tmp $BASE_DIR/config.fio
		echo "" >> $BASE_DIR/config.fio
		#Set io depth to $IODEPTH per device
		qd=$(( $IODEPTH * $k ))
		filename=$(create_fio_config $k $PLUGIN "$disk_names" "$disks_numa" "$cores")
		desc="${BLK_SIZE}BS ${MIX}% MIX $RW IOPS test with $PLUGIN fio_plugin"
		run_spdk_nvme_fio $PLUGIN "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
		 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--output=$NVME_FIO_RESULTS" "--time_based=1"\
		 "--description=$desc"

		#Store values for every number of used disks
		iops_disks[$k]=$((${iops_disks[$k]} + $(get_results iops $MIX)))
		mean_lat_disks[$k]=$((${mean_lat_disks[$k]} + $(get_results mean_lat $MIX)))
		p99_lat_disks[$k]=$((${p99_lat_disks[$k]} + $(get_results p99_lat $MIX)))
		p99_99_lat_disks[$k]=$((${p99_99_lat_disks[$k]} + $(get_results p99_99_lat $MIX)))
		stdev_disks[$k]=$((${stdev_disks[$k]} + $(get_results stdev $MIX)))
		cp $NVME_FIO_RESULTS $BASE_DIR/results/perf_results_${MIX}_${PLUGIN}_${no_cores}cpus_${date}_${k}_disks_${j}.json
		cp $BASE_DIR/config.fio $BASE_DIR/results/config_${MIX}_${PLUGIN}_${no_cores}cpus_${date}_${k}_disks_${j}.fio
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
	iops_disks[$k]=$((${iops_disks[$k]} / 3))
	mean_lat_disks[$k]=$((${mean_lat_disks[$k]} / 3))
	p99_lat_disks[$k]=$((${p99_lat_disks[$k]} / 3))
	p99_99_lat_disks[$k]=$((${p99_99_lat_disks[$k]} / 3))
	stdev_disks[$k]=$((${stdev_disks[$k]} / 3))

	printf "%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} ${mean_lat_disks[$k]} ${p99_lat_disks[$k]}\
	 ${p99_99_lat_disks[$k]} ${stdev_disks[$k]} >> $result_file

	#if tested on only one numeber of disk
	if $ONEWORKLOAD; then
		break
	fi
done
rm -f $BASE_DIR/bdev.conf $BASE_DIR/config.fio
