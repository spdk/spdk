#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR_NVME=$rootdir/examples/nvme/fio_plugin
. $rootdir/scripts/common.sh || exit 1
. $rootdir/test/common/autotest_common.sh
preconditioning=true
FIO_BIN="/usr/src/fio/fio"
nvme_fio_results=$BASE_DIR/result.json

RUNTIME=600
PLUGIN="nvme"
RAMP_TIME=30
BLK_SIZE=4096
RW=randrw
MIX=(100 70 0)
TYPE=("read" "mix" "write")
IODEPTH=(256 32 256)
DISKNO=1
CPU_MASKS=(0x02 0x06 0x1E 0x7E 0x1FE 0x7FE)
CPUs=(1 2 4 6 8 10)

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
	echo "    --no-preconditioning  Skip preconditioning. [deault=Disabled]"
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

	unset iops mean_lat p99_lat p99_99_lat stdev
	echo "CPUs;iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc2_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
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
					filename=$(get_filename $l $PLUGIN "$disk_names")
					run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
					 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"
					iops[$j]=$(get_value iops ${MIX[$i]})
					#search the number of disks where cpus are saturated
					if [[ $max_iops < ${iops[$j]} ]]; then
						max_iops[$j]=${iops[$j]}
						max_disk=$l
						mean_lat[$j]=$(get_value mean_lat ${MIX[$i]})
						p99_lat[$j]=$(get_value p99_lat ${MIX[$i]})
						p99_99_lat[$j]=$(get_value p99_99_lat ${MIX[$i]})
						stdev[$j]=$(get_value stdev ${MIX[$i]})
					fi
				done
				iops[$j]=$max_iops
			else
				filename=$(get_filename $max_disk $PLUGIN $disk_names)
				run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
				 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"

				iops[$j]=$(get_value iops ${MIX[$i]})
				mean_lat[$j]=$(get_value mean_lat ${MIX[$i]})
				p99_lat[$j]=$(get_value p99_lat ${MIX[$i]})
				p99_99_lat[$j]=$(get_value p99_99_lat ${MIX[$i]})
				stdev[$j]=$(get_value stdev ${MIX[$i]})
			fi

			iops[$j]=$((${iops[$j]} + $(get_value iops ${MIX[$i]})))
			mean_lat[$j]=$((${mean_lat[$j]} + $(get_value mean_lat ${MIX[$i]})))
			p99_lat[$j]=$((${p99_lat[$j]} + $(get_value p99_lat ${MIX[$i]})))
			p99_99_lat[$j]=$((${p99_99_lat[$j]} + $(get_value p99_99_lat ${MIX[$i]})))
			stdev[$j]=$((${stdev[$j]} + $(get_value stdev ${MIX[$i]})))
		done
	done
	for (( k=0; k < 6; k++ ))
		do
			iops[$k]=$((${iops[$k]} / 3))
			mean_lat[$k]=$((${mean_lat[$k]} / 3))
			p99_lat[$k]=$((${p99_lat[$k]} / 3))
			p99_99_lat[$k]=$((${p99_99_lat[$k]} / 3))
			stdev[$k]=$((${stdev[$k]} / 3))

			printf "%s,%s,%s,%s,%s,%s\n" ${CPUs[k]} ${iops[$k]} $((${mean_lat[$k]} / 1000)) $((${p99_lat[$k]} / 1000))\
			 $((${p99_99_lat[$k]} / 1000)) $((${stdev[$k]} / 1000)) >> $BASE_DIR/results/tc2_perf_results_${PLUGIN}_${TYPE[$i]}_${date}.csv
		done
done
