#!/usr/bin/env bash

set -e

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_BIN="/usr/src/fio/fio"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help            Print help and exit"
	echo "    --bs=INT          The block size in bytes used for I/O units."
	echo "    --rw=STR          Type of I/O pattern."
	echo "    --rwmixread=INT   Percentage of a mixed workload that should be reads."
	echo "    --iodepth=INT     Number of I/O units to keep in flight against the file."
	echo "    --runtime=TIME    Tell fio to terminate processing after the specified period of time."
	echo "    --ramp_time=TIME  If set, fio will run the specified workload for this amount of time before logging any performance numbers."
	echo "    --fiobin=PATH     Path to fio binary."
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

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)


source $COMMON_DIR/common.sh
trap 'rm -f *.state; error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

PLUGIN_DIR_NVME=$ROOT_DIR/examples/nvme/fio_plugin
PLUGIN_DIR_BDEV=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
nvme_fio_results="$BASE_DIR/nvme_fio"
virtio_fio_results="$BASE_DIR/virtio_fio"
virtio_bdevs=""
results=""

function run_spdk_virtio_fio() {
	LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/perf.fio --ioengine=spdk_bdev\
	 --spdk_conf=$BASE_DIR/bdev.conf "$@" --spdk_mem=1024 -output=$virtio_fio_results\
	 --output-format=json --cpumask=3
}

function run_spdk_nvme_fio(){
	$FIO_BIN $BASE_DIR/perf.fio -output=$nvme_fio_results --output-format=json\
	 "$@" -ioengine=$PLUGIN_DIR_NVME/fio_plugin --cpumask=1 
}

name=$(lspci | grep -i Non | awk '{print $1}')
name=${name//[:]/.}
filename='trtype=PCIe traddr=0000.'${name}' ns=1'

run_spdk_nvme_fio --filename="$filename" --runtime=$RUNTIME --iodepth=$IODEPTH --bs=$BLK_SIZE -rw=$RW  --rwmixread=$MIX --ramp_time=$RAMP_TIME
spdk_vhost_run --conf-path=$BASE_DIR

$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1.0
$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

run_spdk_virtio_fio  --filename=$virtio_bdevs  --runtime=$RUNTIME --bs=$BLK_SIZE --rw=$RW --rwmixread=$MIX --ramp_time=$RAMP_TIME\
	  --iodepth=$IODEPTH

rm -f *.state
spdk_vhost_kill

set +x
results=($(cat $nvme_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.900000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.900000"'))

type=(IOPS BW MEAN p90 p99)
printf "%10s %20s %20s\n" "" "NVMe PMD"     "Virtio"
for i in {0..4}
do
	printf "%10s: %20s %20s\n" ${type[i]} ${results[i * 2]} ${results[i * 2 + 1]}
done
