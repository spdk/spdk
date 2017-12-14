#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"
FIO_BIN="/usr/src/fio/fio"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--fiobin]"
	echo "-h, --help         Print help and exit"
	echo "    --fiobin=PATH  Path to fio binary on host"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			os=*) os_image="${OPTARG#*=}" ;;
			fiobin=*) fio_bin="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $COMMON_DIR/common.sh

if [ ! -x $fio_bin ]; then
	error "Invalid path of fio binary"
fi

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	print_backtrace
	spdk_vhost_kill
	rm -f *.state
	rm -f $BASE_DIR/bdev.conf
	exit 1
}

trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR
function run_fio() {
        LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_BIN --ioengine=spdk_bdev\
         --iodepth=128 --bs=4k --runtime=10 "$@" --spdk_mem=1024
}

function create_bdev_config()
{
	local malloc_name=""

	if ! $RPC_PY get_bdevs | jq -r '.[] .name' | grep -qi "Nvme0n1"$; then
		error "Nvme0n1 bdev not found!"
	fi

	cp $BASE_DIR/bdev.conf.in $BASE_DIR/bdev.conf
	$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1.0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
	sed -i "s|/tmp/vhost.0|$ROOT_DIR/../vhost/naa.Nvme0n1.0|g" $BASE_DIR/bdev.conf

	malloc_name=$($RPC_PY construct_malloc_bdev 128 512)
	$RPC_PY construct_vhost_scsi_controller naa."$malloc_name".1
	$RPC_PY add_vhost_scsi_lun naa."$malloc_name".1 0 $malloc_name
	sed -i "s|/tmp/vhost.1|$ROOT_DIR/../vhost/naa."$malloc_name".1|g" $BASE_DIR/bdev.conf

	malloc_name=$($RPC_PY construct_malloc_bdev 128 4096)
	$RPC_PY construct_vhost_scsi_controller naa."$malloc_name".2
	$RPC_PY add_vhost_scsi_lun naa."$malloc_name".2 0 $malloc_name
	sed -i "s|/tmp/vhost.2|$ROOT_DIR/../vhost/naa."$malloc_name".2|g" $BASE_DIR/bdev.conf
}

timing_enter fio
spdk_vhost_run $BASE_DIR

create_bdev_config
run_fio $BASE_DIR/bdev.fio --filename=VirtioScsi0t0:VirtioScsi1t0:VirtioScsi2t0 --spdk_conf=$BASE_DIR/bdev.conf
run_fio $BASE_DIR/bdev_4G.fio --filename=VirtioScsi0t0 --spdk_conf=$BASE_DIR/bdev.conf

rm -f *.state
rm -f $BASE_DIR/bdev.conf
spdk_vhost_kill
timing_exit fio
