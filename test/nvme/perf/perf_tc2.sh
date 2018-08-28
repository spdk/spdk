#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

. $BASE_DIR/common.sh

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

	unset iops mean_lat p99_lat p99_99_lat stdev
	result_file=$BASE_DIR/results/tc2_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
	echo "run time,ramp time,fio plugin,QD,BS" >> $result_file
	printf "%s,%s,%s,%s,%s,\n" $RUNTIME $RAMP_TIME $PLUGIN ${IODEPTH[$i]} $BLK_SIZE >> $result_file
	echo "CPUs,iops,avg_lat[usec],p99[usec],p99.99[usec],stdev[usec]" >> $result_file
	# Run tests for 1, 2, 4, 6 ,8 ,10 cpu cores
	for (( j=0; j < 6; j++ ))
	do
		CPUMASK=${CPU_MASKS[$j]}
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
					filename=$(create_fio_filename $l $PLUGIN "$disk_names")
					run_spdk_nvme_fio $PLUGIN --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
					 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$NVME_FIO_RESULTS"
					iops[$j]=$(get_results iops ${MIX[$i]})
					#search the number of disks where cpus are saturated
					if [[ $max_iops < ${iops[$j]} ]]; then
						max_iops=${iops[$j]}
						max_disk=$l
						mean_lat[$j]=$(get_results mean_lat ${MIX[$i]})
						p99_lat[$j]=$(get_results p99_lat ${MIX[$i]})
						p99_99_lat[$j]=$(get_results p99_99_lat ${MIX[$i]})
						stdev[$j]=$(get_results stdev ${MIX[$i]})
					fi
				done
				iops[$j]=$max_iops
			else
				filename=$(create_fio_filename $max_disk $PLUGIN "$disk_names")
				run_spdk_nvme_fio $PLUGIN --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
				 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$NVME_FIO_RESULTS"

				iops[$j]=$(get_results iops ${MIX[$i]})
				mean_lat[$j]=$(get_results mean_lat ${MIX[$i]})
				p99_lat[$j]=$(get_results p99_lat ${MIX[$i]})
				p99_99_lat[$j]=$(get_results p99_99_lat ${MIX[$i]})
				stdev[$j]=$(get_results stdev ${MIX[$i]})
			fi

			iops[$j]=$((${iops[$j]} + $(get_results iops ${MIX[$i]})))
			mean_lat[$j]=$((${mean_lat[$j]} + $(get_results mean_lat ${MIX[$i]})))
			p99_lat[$j]=$((${p99_lat[$j]} + $(get_results p99_lat ${MIX[$i]})))
			p99_99_lat[$j]=$((${p99_99_lat[$j]} + $(get_results p99_99_lat ${MIX[$i]})))
			stdev[$j]=$((${stdev[$j]} + $(get_results stdev ${MIX[$i]})))
		done
	done
	for (( k=0; k < 6; k++ ))
		do
			iops[$k]=$((${iops[$k]} / 3))
			mean_lat[$k]=$((${mean_lat[$k]} / 3))
			p99_lat[$k]=$((${p99_lat[$k]} / 3))
			p99_99_lat[$k]=$((${p99_99_lat[$k]} / 3))
			stdev[$k]=$((${stdev[$k]} / 3))

			printf "%s,%s,%s,%s,%s,%s\n" ${CPUs[k]} ${iops[$k]} ${mean_lat[$k]} ${p99_lat[$k]}\
			 ${p99_99_lat[$k]} ${stdev[$k]} >> $result_file
		done
done
