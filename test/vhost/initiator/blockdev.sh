#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"
FIO_BIN="/usr/src/fio/fio"
BDEV_FIO="$BASE_DIR/bdev.fio"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help         Print help and exit"
	echo "    --fiobin=PATH  Path to fio binary on host [default=/usr/src/fio/fio]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $COMMON_DIR/common.sh

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	BDEV_FIO="$BASE_DIR/bdev_nightly.fio"
fi

trap 'rm -f *.state; error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT
function run_spdk_fio() {
        LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_BIN --ioengine=spdk_bdev\
         --runtime=10 "$@" --spdk_mem=1024
}

function create_bdev_config()
{
	if [ -z "$($RPC_PY get_bdevs | jq '.[] | select(.name=="Nvme0n1")')" ]; then
		error "Nvme0n1 bdev not found!"
	fi

	$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1.0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1

	$RPC_PY construct_malloc_bdev 128 512 --name Malloc0
	$RPC_PY construct_vhost_scsi_controller naa.Malloc0.1
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.1 0 Malloc0

	$RPC_PY construct_malloc_bdev 128 4096 --name Malloc1
	$RPC_PY construct_vhost_scsi_controller naa.Malloc1.2
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.2 0 Malloc1
}

timing_enter spdk_vhost_run
spdk_vhost_run $BASE_DIR
timing_exit spdk_vhost_run

timing_enter create_bdev_config
create_bdev_config
timing_exit create_bdev_config

timing_enter run_spdk_fio
run_spdk_fio $BDEV_FIO --filename=VirtioScsi0t0:VirtioScsi1t0:VirtioScsi2t0 \
 --io_size=400m --size=100m --spdk_conf=$BASE_DIR/bdev.conf
timing_exit run_spdk_fio

timing_enter run_spdk_fio_4G
run_spdk_fio $BDEV_FIO --filename=VirtioScsi0t0 \
 --io_size=4G --size=1G --offset=4G --spdk_conf=$BASE_DIR/bdev.conf
timing_exit run_spdk_fio_4G

rm -f *.state
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
