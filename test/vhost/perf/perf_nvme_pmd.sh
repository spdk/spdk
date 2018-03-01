#!/usr/bin/env bash

set -e

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_BIN="/usr/src/fio/fio"
diskno="1"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help            Print help and exit"
	echo "    --bs=INT          The block size in bytes used for I/O units. [default=$BLK_SIZE]"
	echo "    --rw=STR          Type of I/O pattern. [default=$RW]"
	echo "    --rwmixread=INT   Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT     Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --runtime=TIME    Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME  If set, fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiobin=PATH     Path to fio binary. [default=$FIO_BIN]"
	echo "    --disk_no=INT,ALL Number of disks to test on, if =ALL then test on all found disk. [default=1]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			bs=*) BLK_SIZE="${OPTARG#*=}" ;;
			rw=*) RW="${OPTARG#*=}" ;;
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramp_time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) diskno="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1
PLUGIN_DIR_NVME=$SPDK_BUILD_DIR/examples/nvme/fio_plugin
nvme_fio_results="$BASE_DIR/nvme_fio"

trap 'rm -f *.state $nvme_fio_results; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

function run_spdk_nvme_fio(){
	$FIO_BIN $BASE_DIR/perf.fio --output-format=json\
	 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin --cpumask=1
}

name=($(lspci | grep -i Non | awk '{print $1}'))
if [[ $diskno =~ ^[0-9]+$ ]]; then
	if [[ $diskno -gt ${#name[@]} ]]; then
		error "Required devices number ($diskno) is larger than the number of devices found (${#name[@]})"
	fi
fi

if [[ $diskno == "ALL" ]] || [[ $diskno == "all" ]]; then
	for i in ${!name[@]}
	do
		name[i]=${name[i]//[:]/.}
		dev_name='trtype=PCIe traddr=0000.'${name[i]}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
else
	for (( i=0; i < $diskno; i++ ))
	do
		name[i]=${name[i]//[:]/.}
		dev_name='trtype=PCIe traddr=0000.'${name[i]}' ns=1'
		filename+=$(printf %s":" "$dev_name")
	done
fi

run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$nvme_fio_results"
