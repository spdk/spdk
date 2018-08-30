#!/usr/bin/env bash

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR_NVME=$rootdir/examples/nvme/fio_plugin
. $rootdir/scripts/common.sh || exit 1
. $rootdir/test/common/autotest_common.sh

preconditioning=true
FIO_BIN="/usr/src/fio/fio"
RUNTIME=3600
RAMP_TIME=300
BLK_SIZE=4096
RW=randrw
MIX=(100 0)
TYPE=("read" "write")
POLLING=("classic" "hybrid" "spdk")
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

trap 'set -e; set_classic_polling; rm -f *.state $nvme_fio_results $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT
. $BASE_DIR/common.sh

if [ $PLUGIN = "bdev" ]; then
	echo "[Nvme]" >> $BASE_DIR/bdev.conf
	$rootdir/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
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
				echo -1 > /sys/block/nvme0n1/queue/io_poll_delay
			fi
	done
}

disk_names=$(get_disks $PLUGIN)
# Bind default linux driver
$rootdir/scripts/setup.sh reset

# Linux Kernel with classic polling, hybrid polling and SPDK
for (( i=0; i < 3; i++ ))
do
	filename=$(get_filename $DISKNO "kernel" "$disk_names")
	fio_cmd="run_nvme_fio"

	if [ "${POLLING[$i]}" = "classic" ]; then
		set_classic_polling
	elif [ "${POLLING[$i]}" = "hybrid" ]; then
		set_hybrid_polling
	else
		HUGEMEM=10240 $rootdir/scripts/setup.sh
		sleep 1

		filename=$(get_filename $DISKNO $PLUGIN "$disk_names")
		fio_cmd="run_spdk_nvme_fio"
	fi
	# Read, Write
	for (( j=0; j < 2; j++ ))
	do
		if [ "${MIX[$j]}" = "0" ] && $preconditioning; then
			preconditioning --write
		elif $preconditioning; then
			preconditioning
		fi

		$fio_cmd --filename="$filename" --runtime=$RUNTIME --bs=$BLK_SIZE --log_avg_msec=250 --ramp_time=$RAMP_TIME\
		 --cpumask=$CPUMASK --output=$nvme_fio_results\
		 --write_lat_log=$BASE_DIR/results/tc3_perf_lat_${TYPE[$j]}_${POLLING[$i]}_$date

		iops=$(get_value iops ${MIX[$j]})
		mean_lat=$(get_value mean_lat ${MIX[$j]})
		p99_lat=$(get_value p99_lat ${MIX[$j]})
		p99_99_lat=$(get_value p99_99_lat ${MIX[$j]})
		stdev=$(get_value stdev ${MIX[$j]})

		echo "iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc3_perf_results_${TYPE[$j]}_${POLLING[$i]}_${date}.csv
		printf "%s,%s,%s,%s,%s\n" $iops $(($mean_lat / 1000)) $(($p99_lat / 1000))\
		 $(($p99_99_lat / 1000)) $(($stdev / 1000)) >> $BASE_DIR/results/tc3_perf_results_${TYPE[$j]}_${POLLING[$i]}_${date}.csv
	done
done

set_classic_polling
