#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

. $BASE_DIR/common.sh

if [ $PLUGIN = "bdev" ]; then
	$ROOTDIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

disk_names=$(get_disks $PLUGIN)
disks_numa=$(get_numa_node $PLUGIN "$disk_names")

if $PRECONDITIONING; then
	preconditioning
fi

unset iops mean_lat p99_lat p99_99_lat stdev
result_file=$BASE_DIR/results/tc2_perf_results_${MIX}_${PLUGIN}_${date}.csv
echo "run time,ramp time,fio plugin,QD,BS" >> $result_file
printf "%s,%s,%s,%s,%s,\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE >> $result_file
echo "CPUs,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec]" >> $result_file
# Measure troughput scalability of SPDK Run tests with 1, 2, 4, 6 ,8 ,10 cpu cores
for (( j=0; j < 6; j++ ))
do
	cpus_allowed=$(create_cpu_allowed ${CPUS[$j]})
	max_iops=0
	max_disk=0
	#Each Workload repeat 3 times
	for (( k=0; k < 3; k++ ))
	do
		# Run test with multiple disk configuration once and find disk number where cpus are saturated, next do runs do with
		# only that disk number.
		if [ "$max_iops" = "0" ]; then
			#Start with $DISKNO disks and remove 2 disks for each run
			for (( l=$DISKNO; l >= 1; l-=2 ))
			do
				#Set io depth to IODEPTH per device
				qd=$(( $IODEPTH * $l ))
				filename=$(create_fio_filename $l $PLUGIN "$disk_names" "$disks_numa")
				run_spdk_nvme_fio $PLUGIN "--filename=$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
				 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--cpus_allowed=$cpus_allowed" "--output=$NVME_FIO_RESULTS"\
				 "--time_based=1"
				iops[$j]=$(get_results iops $MIX)
				#search the number of disks where cpus are saturated
				if [[ $max_iops < ${iops[$j]} ]]; then
					max_iops=${iops[$j]}
					max_disk=$l
					mean_lat[$j]=$(get_results mean_lat $MIX)
					p99_lat[$j]=$(get_results p99_lat $MIX)
					p99_99_lat[$j]=$(get_results p99_99_lat $MIX)
					stdev[$j]=$(get_results stdev $MIX)
				fi
			done
			iops[$j]=$max_iops
		else
			qd=$(( $IODEPTH * $max_disk ))
			filename=$(create_fio_filename $max_disk $PLUGIN "$disk_names" "$disks_numa")
			run_spdk_nvme_fio $PLUGIN -"-filename=$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
			 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$qd" "--cpus_allowed=$cpus_allowed" "--output=$NVME_FIO_RESULTS"\
			 "--time_based=1"

			iops[$j]=$(get_results iops $MIX)
			mean_lat[$j]=$(get_results mean_lat $MIX)
			p99_lat[$j]=$(get_results p99_lat $MIX)
			p99_99_lat[$j]=$(get_results p99_99_lat $MIX)
			stdev[$j]=$(get_results stdev $MIX)
		fi

		iops[$j]=$((${iops[$j]} + $(get_results iops $MIX)))
		mean_lat[$j]=$((${mean_lat[$j]} + $(get_results mean_lat $MIX)))
		p99_lat[$j]=$((${p99_lat[$j]} + $(get_results p99_lat $MIX)))
		p99_99_lat[$j]=$((${p99_99_lat[$j]} + $(get_results p99_99_lat $MIX)))
		stdev[$j]=$((${stdev[$j]} + $(get_results stdev $MIX)))
	done
done
for (( k=0; k < 6; k++ ))
do
	iops[$k]=$((${iops[$k]} / 3))
	mean_lat[$k]=$((${mean_lat[$k]} / 3))
	p99_lat[$k]=$((${p99_lat[$k]} / 3))
	p99_99_lat[$k]=$((${p99_99_lat[$k]} / 3))
	stdev[$k]=$((${stdev[$k]} / 3))

	printf "%s,%s,%s,%s,%s,%s\n" ${CPUS[k]} ${iops[$k]} ${mean_lat[$k]} ${p99_lat[$k]}\
	 ${p99_99_lat[$k]} ${stdev[$k]} >> $result_file
done

rm -f $BASE_DIR/bdev.conf
