#!/usr/bin/env bash

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

TC4=1
. $BASE_DIR/common.sh

MIX=(100 0 70)
QD=(1 2 4 8 16 32 64 128)
DRIVER=("kernel" "spdk")

if [ $PLUGIN = "bdev" ]; then
	$ROOTDIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

#dpdk launched with fio_plugin use core 0x01, lets not use it for fio run
CPU_MASK="0x"$(printf "%x\n" $(( ((1 << $CPU_NO) -1) << 1)))

disk_names=$(get_disks $PLUGIN)
# Bind default linux driver
$ROOTDIR/scripts/setup.sh reset

#Kernel driver, spdk
for (( i=0; i < 2; i++ ))
do
	if [ "${DRIVER[$i]}" = "kernel" ]; then
		filename=$(create_fio_filename $DISKNO "kernel")
		fio_cmd="run_nvme_fio"
	else
		HUGEMEM=10240 $ROOTDIR/scripts/setup.sh
		sleep 1

		filename=$(create_fio_filename $DISKNO $PLUGIN "$disk_names")
		fio_cmd="run_spdk_nvme_fio $PLUGIN"
	fi
	#Read, write, read/write mix
	for (( j=0; j < 3; j++ ))
	do
		result_file=$BASE_DIR/results/tc4_perf_results_${TYPE[$j]}_${DRIVER[$i]}_${date}.csv
		echo "run time,ramp time,fio plugin,BS" >> $result_file
		printf "%s,%s,%s,%s\n" $RUNTIME $RAMP_TIME $PLUGIN $BLK_SIZE >> $result_file
		echo "QD;iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $result_file
		#queue depth
		for (( k=0; k < 8; k++))
		do
			$fio_cmd --filename="$filename" --runtime=$RUNTIME --bs=$BLK_SIZE --ramp_time=$RAMP_TIME\
			 --rw=$RW --rwmixread=${MIX[$j]} --iodepth=${QD[$k]} --cpumask=$CPU_MASK --output=$NVME_FIO_RESULTS

			iops=$(get_results iops ${MIX[$j]})
			mean_lat=$(get_results mean_lat ${MIX[$j]})
			p99_lat=$(get_results p99_lat ${MIX[$j]})
			p99_99_lat=$(get_results p99_99_lat ${MIX[$j]})
			stdev=$(get_results stdev ${MIX[$j]})

			printf "%s %s,%s,%s,%s,%s\n" ${QD[$k]} $iops $mean_lat $p99_lat \
			 $p99_99_lat $stdev >> $result_file
		done
	done
done
