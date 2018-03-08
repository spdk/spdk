#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_BIN="/usr/src/fio/fio"
virtio_fio_results="$BASE_DIR/virtio_fio"
nvme_fio_results="$BASE_DIR/nvme_fio"
results=""

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
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

source $COMMON_DIR/common.sh
trap 'rm -f *.state $nvme_fio_results $virtio_fio_results; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

timing_enter spdk_nvme_fio
. /$BASE_DIR/perf_nvne_pmd.sh "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$nvme_fio_results"
timing_exit spdk_nvme_fio

timing_enter spdk_virtio_fio
. /$BASE_DIR/perf_virtio_initiator.sh "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$virtio_fio_results"
timing_enter spdk_virtio_fio

set +x
results=($(cat $nvme_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.000000"'))

type=(IOPS BW MEAN p90 p99)
printf "%10s %20s %20s\n" "" "NVMe PMD"     "Virtio"
for i in {0..4}
do
	printf "%10s: %20s %20s\n" ${type[i]} ${results[i * 2]} ${results[i * 2 + 1]}
done

rm -f *.state $nvme_fio_results $virtio_fio_results
