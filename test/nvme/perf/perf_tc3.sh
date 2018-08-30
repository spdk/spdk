#!/usr/bin/env bash

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

. $BASE_DIR/common.sh
MIX=(100 0)
TYPE=("randread" "randwrite")
CPUMASK=0x02
POLLING=("classic" "hybrid" "spdk")
IODEPTH=1

if [ $PLUGIN = "bdev" ]; then
	$ROOTDIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

function set_classic_polling()
{
	local i
	for (( i=0; j<$DISKNO; j++ ))
		do
			if [ -b "/dev/nvme${i}n1" ]; then
				echo -1 > /sys/block/nvme0n1/queue/io_poll_delay
			fi
	done
}

function set_hybrid_polling()
{
	local i
	for (( i=0; j<$DISKNO; j++ ))
		do
			if [ -b "/dev/nvme${i}n1" ]; then
				echo 0 > /sys/block/nvme0n1/queue/io_poll_delay
			fi
	done
}

trap 'set -e; set_classic_polling; rm -f *.state $nvme_fio_results $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
disk_names=$(get_disks $PLUGIN)
# Bind default linux driver
$ROOTDIR/scripts/setup.sh reset

# Measure and compare latencies of Linux kernel driver with classic or hybrid polling and SPDK driver
for (( i=0; i < 3; i++ ))
do

	if [ "${POLLING[$i]}" = "classic" ]; then
		filename=$(create_fio_filename $DISKNO "kernel" "$disk_names")
		fio_cmd="run_nvme_fio --ioengine=libaio"
		set_classic_polling
	elif [ "${POLLING[$i]}" = "hybrid" ]; then
		filename=$(create_fio_filename $DISKNO "kernel" "$disk_names")
		fio_cmd="run_nvme_fio --ioengine=pvsync2 --hipri=100"
		set_hybrid_polling
	else
		HUGEMEM=10240 $ROOTDIR/scripts/setup.sh
		sleep 1

		filename=$(create_fio_filename $DISKNO $PLUGIN "$disk_names")
		fio_cmd="run_spdk_nvme_fio $PLUGIN"
	fi
	# Randread, randwrite
	for (( j=0; j < 2; j++ ))
	do
		if $PRECONDITIONING; then
			preconditioning
		fi

		$fio_cmd --filename="$filename" --runtime=$RUNTIME --bs=$BLK_SIZE --log_avg_msec=250 --ramp_time=$RAMP_TIME\
		 --iodepth=$IODEPTH --cpumask=$CPUMASK --output=$NVME_FIO_RESULTS\
		 --write_lat_log=$BASE_DIR/results/tc3_perf_lat_${TYPE[$j]}_${POLLING[$i]}_$date

		iops=$(get_results iops ${MIX[$j]})
		mean_lat=$(get_results mean_lat ${MIX[$j]})
		p99_lat=$(get_results p99_lat ${MIX[$j]})
		p99_99_lat=$(get_results p99_99_lat ${MIX[$j]})
		stdev=$(get_results stdev ${MIX[$j]})

		result_file=$BASE_DIR/results/tc3_perf_results_${TYPE[$j]}_${POLLING[$i]}_${date}.csv
		echo "run time,ramp time,fio plugin,QD,BS" >> $result_file
		printf "%s,%s,%s,%s,%s,\n" $RUNTIME $RAMP_TIME $PLUGIN $IODEPTH $BLK_SIZE >> $result_file
		echo "iops,avg_lat[u],p99[u],p99.99[u],stdev[u]" >> $result_file
		printf "%s,%s,%s,%s,%s\n" $iops $mean_lat $p99_lat\
		 $p99_99_lat $stdev >> $result_file
	done
done
