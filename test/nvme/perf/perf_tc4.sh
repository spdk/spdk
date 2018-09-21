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
MIX=(100 0 70)
QD=(1 2 4 8 16 32 64 128)
TYPE=("read" "write" "mix")
DRIVER=("kernel" "spdk")
CPU_NO=2
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
	echo "    --cpu_no=INT      Number of cores to test with. [default=$CPU_NO]"
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
			cpu_no=*) CPU_NO="${OPTARG#*=}" ;;
			no-preconditioning) preconditioning=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

. $BASE_DIR/common.sh
trap 'rm -f *.state $nvme_fio_results $BASE_DIR/bdev.conf; print_backtrace' ERR SIGTERM SIGABRT

if [ $PLUGIN = "bdev" ]; then
	echo "[Nvme]" >> $BASE_DIR/bdev.conf
	$rootdir/scripts/gen_nvme.sh >> $BASE_DIR/bdev.conf
fi

#dpdk launched with fio_plugin use core 0x01, lets not use it for fio run
CPU_MASK="0x"$(printf "%x\n" $(( ((1 << $CPU_NO) -1) << 1)))

disk_names=$(get_disks $PLUGIN)
# Bind default linux driver
$rootdir/scripts/setup.sh reset

#Kernel driver, spdk
for (( i=0; i < 2; i++ ))
do
	if [ "${DRIVER[$i]}" = "kernel" ]; then
		filename=$(get_filename $DISKNO "kernel")
		fio_cmd="run_nvme_fio"
	else
		HUGEMEM=10240 $rootdir/scripts/setup.sh
		sleep 1

		filename=$(get_filename $DISKNO $PLUGIN "$disk_names")
		fio_cmd="run_spdk_nvme_fio"
	fi
	#Read, write, read/write mix
	for (( j=0; j < 3; j++ ))
	do
		echo "QD;iops;avg_lat[u];p99[u];p99.99[u];stdev[u]" >> $BASE_DIR/results/tc4_perf_results_${TYPE[$j]}_${DRIVER[$i]}_${date}.csv
		#queue depth
		for (( k=0; k < 8; k++))
		do
			$fio_cmd --filename="$filename" --runtime=$RUNTIME --bs=$BLK_SIZE --ramp_time=$RAMP_TIME\
			 --rw=$RW --rwmixread=${MIX[$j]} --iodepth=${QD[$k]} --cpumask=$CPU_MASK --output=$nvme_fio_results

			iops=$(get_value iops ${MIX[$j]})
			mean_lat=$(get_value mean_lat ${MIX[$j]})
			p99_lat=$(get_value p99_lat ${MIX[$j]})
			p99_99_lat=$(get_value p99_99_lat ${MIX[$j]})
			stdev=$(get_value stdev ${MIX[$j]})

			printf "%s %s,%s,%s,%s,%s\n" ${QD[$k]} $iops $(($mean_lat / 1000)) $(($p99_lat / 1000))\
			 $(($p99_99_lat / 1000)) $(($stdev / 1000)) >> $BASE_DIR/results/tc4_perf_results_${TYPE[$j]}_${DRIVER[$i]}_${date}.csv
		done
	done
done
