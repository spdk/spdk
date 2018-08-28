#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

. $BASE_DIR/common.sh
CPUMASK=0x02

if [ $PLUGIN = "bdev" ]; then
	$ROOTDIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

disk_names=$(get_disks $PLUGIN)
#100% READ, 70% READ and 30% WRITE, 100% WRITE
for (( i=0; i < 3; i++ ))
do
	if $PRECONDITIONING; then
		preconditioning
	fi

	result_file=$BASE_DIR/results/tc1_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
	unset iops_disks mean_lat_disks p99_lat_disks p99_99_lat_disks stdev_disks
	echo "run time,ramp time,fio plugin,QD,BS" > $result_file
	printf "%s,%s,%s,%s,%s,\n" $RUNTIME $RAMP_TIME $PLUGIN ${IODEPTH[$i]} $BLK_SIZE >> $result_file
	echo "no_of_disks,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec]" >> $result_file
	#Each Workload repeat 3 times
	for (( j=0; j < 3; j++ ))
	do
		#Start with $DISKNO disks and remove 2 disks for each run
		for (( k=$DISKNO; k >= 1; k-=2 ))
		do
			filename=$(create_fio_filename $k $PLUGIN "$disk_names")
			run_spdk_nvme_fio $PLUGIN --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$NVME_FIO_RESULTS"

			#Store values for every number of used disks
			iops_disks[$k]=$((${iops_disks[$k]} + $(get_results iops ${MIX[$i]})))
			mean_lat_disks[$k]=$((${mean_lat_disks[$k]} + $(get_results mean_lat ${MIX[$i]})))
			p99_lat_disks[$k]=$((${p99_lat_disks[$k]} + $(get_results p99_lat ${MIX[$i]})))
			p99_99_lat_disks[$k]=$((${p99_99_lat_disks[$k]} + $(get_results p99_99_lat ${MIX[$i]})))
			stdev_disks[$k]=$((${stdev_disks[$k]} + $(get_results stdev ${MIX[$i]})))
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
	done
done

rm -f $BASE_DIR/bdev.conf
