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
BLK_SIZE=4096
RW=randrw
MIX=(100 0 70)
IODEPTH=(256 32 256)
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
	echo "    --fiobin=PATH         Path to fio binary. [default=$FIO_BIN]"
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
			no-preconditioning) preconditioning=false ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

function run_spdk_nvme_fio(){
LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/config.fio --output-format=json\
	 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin 
}

trap 'rm -f *.state $nvme_fio_results; print_backtrace' ERR SIGTERM SIGABRT

disks=($(iter_pci_class_code 01 08 02))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
fi

function preconditioning(){
	dev_name=""
	filename=""
	for (( i=0; i < $DISKNO; i++ ))
	do
		dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done

	if [ "$1" = "--write" ]; then
		run_spdk_nvme_fio --filename="$filename" "--runtime=5400" "--bs=4096"\
	 	"--rw=randwrite" "--iodepth=32"
	else
		run_spdk_nvme_fio --filename="$filename" "--runtime=1200" "--bs=131072"\
	 	"--rw=write" "--iodepth=32"
	fi
}


#100% READ, 100% WRITE, 70% READ and 30% WRITE
for (( i=0; i < 3; i++ ))
do
	if [ "${MIX[$i]}" = "0" ] && $preconditioning; then
		preconditioning --write
	elif $preconditioning; then
		preconditioning
	fi

	#Each Workload repeat 3 times
	for (( j=0; j < 3; j++ ))
	do
		count=0
		avg_mean_iops=0
		avg_mean_lat=0
		#Start with $DISKNO disks and remove 2 disks for each run
		for (( k=$DISKNO; k >= 1; k-=2 ))
		do
			dev_name=""
			filename=""
			for (( l=0; l < $k; l++ ))
			do
				dev_name='trtype=PCIe traddr='${disks[l]//:/.}' ns=1'
				filename+=$(printf %s":" "$dev_name")
			done
			
			run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--bs=$BLK_SIZE"\
			 "--rw=$RW" "--rwmixread=${MIX[$i]}" "--iodepth=${IODEPTH[$i]}" "--cpumask=$CPUMASK" "--output=$nvme_fio_results"
			iops=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.iops + .write.iops)')
			iops=${iops%.*}
			mean_lat=$(cat $nvme_fio_results | jq -r '.jobs[] | (.read.clat_ns.mean + .write.clat_ns.mean)')
			mean_lat=${mean_lat%.*}
			
			count=$(($count + 1))
			avg_mean_iops=$(($avg_mean_iops + $iops))
			avg_mean_lat=$(($avg_mean_lat + $mean_lat))
		done

		avg_mean_iops=$(($avg_mean_iops / $count))
		avg_mean_lat=$(($avg_mean_lat / $count))
		avg_avg_mean_lat[i]=$((${avg_avg_mean_lat[$i]} + $avg_mean_lat))
		aggr_iops_count[i]=$((${aggr_iops_count[$i]} + $avg_mean_iops))
	done
done

date="$(date +'%m_%d_%Y_%H%M%S')"
mkdir -p $BASE_DIR/results
touch $BASE_DIR/results/perf_results_${date}.csv
echo "IOPS[100% read],IOPS[100% write],IOPS[70% read],AVG_LAT[100% read],AVG_LAT[100% write],AVG_LAT[70% read]"\
 >> $BASE_DIR/results/perf_results_${date}.csv
printf "%s,%s,%s,%s,%s,%s\n" ${aggr_iops_count[0]} ${aggr_iops_count[1]} ${aggr_iops_count[2]}\
 ${avg_avg_mean_lat[0]} ${avg_avg_mean_lat[1]} ${avg_avg_mean_lat[2]} >> $BASE_DIR/results/perf_results_${date}.csv

