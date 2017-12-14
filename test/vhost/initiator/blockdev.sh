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
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help         Print help and exit"
	echo "    --fiobin=PATH  Path to fio binary on host [default=/usr/src/fio/fio]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiobin=*) fio_bin="${OPTARG#*=}" ;;
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

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	print_backtrace
	spdk_vhost_kill
	rm -f *.state
	exit 1
}

trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR
function run_fio() {
        LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_BIN --ioengine=spdk_bdev\
         --iodepth=128 --bs=4k --runtime=10 "$@" --spdk_mem=1024
}

function create_bdev_config()
{
	if ! $RPC_PY get_bdevs | jq -r '.[] .name' | grep -qi "Nvme0n1"$; then
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

timing_enter fio
spdk_vhost_run $BASE_DIR

create_bdev_config
run_fio $BASE_DIR/bdev.fio --filename=VirtioScsi0t0:VirtioScsi1t0:VirtioScsi2t0 --io_size=400m --size=100m --spdk_conf=$BASE_DIR/bdev.conf
run_fio $BASE_DIR/bdev.fio --filename=VirtioScsi0t0 --io_size=4G --size=1G --offset=4G --spdk_conf=$BASE_DIR/bdev.conf

rm -f *.state
spdk_vhost_kill
timing_exit fio
