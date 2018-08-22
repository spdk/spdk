#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
ROOTDIR=$(readlink -f $BASE_DIR/../../..)

PRECONDITIONING=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=(100 70 0)
TYPE=("randread" "randrw" "randwrite")
IODEPTH=(256 256 32)
CPUMASK=0x02
DISKNO=1

function usage()
{
	set +x
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Run Nvme PMD/BDEV performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --run-time=TIME[s]    Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp-time=TIME[s]   Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fio-bin=PATH        Path to fio binary. [default=$FIO_BIN]"
	echo "    --fio-plugin=STR      Use bdev or nvme fio_plugin. [default=$PLUGIN]"
	echo "    --disk-no=INT,ALL     Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --no-preconditioning  Skip preconditioning"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			run-time=*) RUNTIME="${OPTARG#*=}" ;;
			ramp-time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fio-bin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk-no=*) DISKNO="${OPTARG#*=}" ;;
			fio-plugin=*) PLUGIN="${OPTARG#*=}" ;;
			no-preconditioning) PRECONDITIONING=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

trap "rm -f *.state $NVME_FIO_RESULTS $BASE_DIR/bdev.conf; print_backtrace" ERR SIGTERM SIGABRT
. $BASE_DIR/common.sh

if [ $PLUGIN = "bdev" ]; then
	echo "[Nvme]" >> $BASE_DIR/bdev.conf
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
