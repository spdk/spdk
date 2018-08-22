#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $BASE_DIR/../../..)

preconditioning=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=300
BLK_SIZE=4096
RW=randrw
MIX=(100 70 0)
TYPE=("read" "mix" "write")
IODEPTH=(256 256 32)
CPUMASK=0x02
DISKNO=1

function usage()
{
	set +x
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Run Nvme PMD performance test. Change options for easier debug and setup configuration"
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --runtime=TIME        Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME      Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiobin=PATH         Path to fio binary. [default=$FIO_BIN]"
	echo "    --fio_plugin=STR      Use bdev or nvme fio_plugin. [default=$PLUGIN]"
	echo "    --disk_no=INT,ALL     Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --no-preconditioning  Skip preconditioning"
	set -x
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramp_time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) DISKNO="${OPTARG#*=}" ;;
			fio_plugin=*) PLUGIN="${OPTARG#*=}" ;;
			no-preconditioning) preconditioning=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

trap 'rm -f *.state $nvme_fio_results $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
. $BASE_DIR/common.sh

if [ $PLUGIN = "bdev" ]; then
	echo "[Nvme]" >> $BASE_DIR/bdev.conf
	$rootdir/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

disk_names=$(get_disks $PLUGIN)
#100% READ, 70% READ and 30% WRITE, 100% WRITE
for (( i=0; i < 3; i++ ))
do
	if [ "${MIX[$i]}" = "0" ] && $preconditioning; then
			preconditioning --write
	elif $preconditioning; then
			preconditioning
	fi

	unset iops_disks mean_lat_disks p99_lat_disks p99_99_lat_disks stdev_disks
	echo "no_of_disks;iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc1_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
	#Each Workload repeat 3 times
	for (( j=0; j < 3; j++ ))
	do
		#Start with $DISKNO disks and remove 2 disks for each run
		for (( k=$DISKNO; k >= 1; k-=2 ))
		do
			filename=$(get_filename $k $PLUGIN $disk_names)
			run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--bs=$BLK_SIZE"\
			 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"

			#Store values for every number of used disks
			iops_disks[$k]=$((${iops_disks[$k]} + $(get_value iops $MIX[$i])))
			mean_lat_disks[$k]=$((${mean_lat_disks[$k]} + $(get_value mean_lat $MIX[$i])))
			p99_lat_disks[$k]=$((${p99_lat_disks[$k]} + $(get_value p99_lat $MIX[$i])))
			p99_99_lat_disks[$k]=$((${p99_99_lat_disks[$k]} + $(get_value p99_99_lat $MIX[$i])))
			stdev_disks[$k]=$((${stdev_disks[$k]} + $(get_value stdev $MIX[$i])))
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

		printf "%s,%s,%s,%s,%s,%s\n" ${k} ${iops_disks[$k]} $((${mean_lat_disks[$k]} / 1000)) $((${p99_lat_disks[$k]} / 1000))\
		 $((${p99_99_lat_disks[$k]} / 1000)) $((${stdev_disks[$k]} / 1000)) >> $BASE_DIR/results/tc1_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
	done
done

rm -f $BASE_DIR/bdev.conf
