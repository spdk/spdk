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
PLUGIN_DIR_BDEV=$SPDK_BUILD_DIR/examples/bdev/fio_plugin
virtio_fio_results="$BASE_DIR/virtio_fio"

trap 'rm -f *.state $virtio_fio_results; rm -f $BASE_DIR/bdev.conf; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

RPC_PY="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
virtio_bdevs=""

function run_spdk_virtio_fio() {
	LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/perf.fio --ioengine=spdk_bdev\
	 --spdk_conf=$BASE_DIR/bdev.conf "$@" --spdk_mem=1024\
	 --output-format=json --cpumask=3
}

timing_enter spdk_vhost_run
spdk_vhost_run --conf-path=$BASE_DIR
timing_exit spdk_vhost_run

name=($(jq -r '.[].name' <<<  $($RPC_PY get_bdevs)))
if [[ $diskno =~ ^[0-9]+$ ]]; then
	if [[ $diskno -gt ${#name[@]} ]]; then
		error "Required devices number ($diskno) is larger than the number of devices found (${#name[@]})"
	fi
fi

touch $BASE_DIR/bdev.conf
if [[ $diskno == "ALL" ]] || [[ $diskno == "all" ]]; then
	for i in ${!name[@]}
	do
		$RPC_PY construct_vhost_scsi_controller naa.${name[i]}.0
		$RPC_PY add_vhost_scsi_lun naa.${name[i]}.0 0 ${name[i]}
		echo "[VirtioUser$i]" >> $BASE_DIR/bdev.conf
		echo "  Path naa.${name[i]}.0" >> $BASE_DIR/bdev.conf
		echo "  Queues 2" >> $BASE_DIR/bdev.conf
		echo "" >> $BASE_DIR/bdev.conf
	done
else
	for (( i=0; i < $diskno; i++ ))
	do
		$RPC_PY construct_vhost_scsi_controller naa.${name[i]}.0
		$RPC_PY add_vhost_scsi_lun naa.${name[i]}.0 0 ${name[i]}
		echo "[VirtioUser$i]" >> $BASE_DIR/bdev.conf
		echo "  Path naa.${name[i]}.0" >> $BASE_DIR/bdev.conf
		echo "  Queues 2" >> $BASE_DIR/bdev.conf
		echo "" >> $BASE_DIR/bdev.conf
	done
fi

vbdevs=$(discover_bdevs $SPDK_BUILD_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

run_spdk_virtio_fio  --filename=$virtio_bdevs  "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$virtio_fio_results"

rm -f $BASE_DIR/bdev.conf
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
