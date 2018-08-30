#!/usr/bin/env bash

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR_NVME=$rootdir/examples/nvme/fio_plugin
. $rootdir/scripts/common.sh || exit 1
. $rootdir/test/common/autotest_common.sh
preconditioning=true
FIO_BIN="/usr/src/fio/fio"
nvme_fio_results=$BASE_DIR/result.json

RUNTIME=3600
RAMP_TIME=300
BLK_SIZE=4096
RW=randrw
MIX=(100 0 70)
TYPE=("read" "write" )
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
			no-preconditioning) preconditioning=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

date="$(date +'%m_%d_%Y_%H%M%S')"
mkdir -p $BASE_DIR/results
trap 'set -e; rm -f *.state $nvme_fio_results; echo -1 > /sys/block/nvme0n1/queue/io_poll_delay; print_backtrace' ERR SIGTERM SIGABRT

function preconditioning(){

	if [ -b "/dev/nvme0n1" ]; then
		pre_cmd="$FIO_BIN $BASE_DIR/config.fio --filename=/dev/nvme0n1 --iodepth=32"
	else
		pre_cmd="LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --filename='trtype=PCIe traddr=0000.0b.00.0 ns=1' --iodepth=32"
	fi
	if [ "$1" = "--write" ]; then
		pre_cmd+="--rw=randwrite --runtime=1200 --bs=4096"
	else
		pre_cmd+="--rw=write --runtime=5400 --bs=131072"
	fi

	bash -c "$fio_cmd"
}

# Bind default linux driver
$rootdir/scripts/setup.sh reset

# Linux Kernel with classic polling, hybrid polling and SPDK
for (( i=0; i < 3; i++ ))
do
	fio_cmd="$FIO_BIN $BASE_DIR/config.fio --runtime=$RUNTIME --bs=$BLK_SIZE --rw=$RW --iodepth=1 --output-format=json --log_avg_msec=250 --ramp_time=$RAMP_TIME\
	 --cpumask=$CPUMASK --ioengine=pvsync2 --hipri=100 --filename=/dev/nvme0n1 --cpus_allowed=28 --output=$nvme_fio_results"

	if [ "${POLLING[$i]}" = "classic" ]; then
		echo -1 > /sys/block/nvme0n1/queue/io_poll_delay
	elif [ "${POLLING[$i]}" = "hybrid" ]; then
		echo 0 > /sys/block/nvme0n1/queue/io_poll_delay
	else
		HUGEMEM=10240 $rootdir/scripts/setup.sh
		fio_cmd="LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio\
	 	 --runtime=$RUNTIME --bs=$BLK_SIZE --rw=$RW --iodepth=1 --filename='trtype=PCIe traddr=0000.0b.00.0 ns=1' --log_avg_msec=250 --ramp_time=$RAMP_TIME\
	 	 --cpumask=$CPUMASK --ioengine=$PLUGIN_DIR_NVME/fio_plugin --output-format=json --output=$nvme_fio_results"
	fi
	# Read, Write
	for (( j=0; j < 2; j++ ))
	do
		if [ "${MIX[$j]}" = "0" ] && $preconditioning; then
			preconditioning --write
		elif $preconditioning; then
			preconditioning
		fi

		fio_cmd+=" --rwmixread=${MIX[$j]} --write_lat_log=$BASE_DIR/results/tc3_perf_lat_${TYPE[$j]}_${POLLING[$i]}_$date"
		bash -c "$fio_cmd"

		iops=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.iops + .write.iops)')
		iops=${iops%.*}
		mean_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
		mean_lat=${mean_lat%.*}
		p99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.000000" + .write.clat_ns.percentile."99.000000")')
		p99_lat=${p99_lat%.*}
		p99_99_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.percentile."99.990000" + .write.clat_ns.percentile."99.990000")')
		p99_99_lat=${p99_99_lat%.*}
		stdev=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.stddev + .write.clat_ns.stddev)')
		stdev=${stdev%.*}

		echo "iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc3_perf_results_${TYPE[$j]}_${POLLING[$i]}_${date}.csv
		printf "%s,%s,%s,%s,%s\n" $iops $(($mean_lat / 1000)) $(($p99_lat / 1000))\
		 $(($p99_99_lat / 1000)) $(($stdev / 1000)) >> $BASE_DIR/results/tc3_perf_results_${TYPE[$j]}_${POLLING[$i]}_${date}.csv
	done
done
echo -1 > /sys/block/nvme0n1/queue/io_poll_delay
