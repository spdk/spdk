#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"
FIO_BIN="/usr/src/fio/fio"
virtio_bdevs=""
virtio_nvme_bdevs=""

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

trap 'rm -f *.state; error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT
function run_spdk_fio() {
	LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_BIN --ioengine=spdk_bdev\
         "$@" --spdk_mem=1024
}

function create_bdev_config()
{
	local vbdevs

	if [ -z "$($RPC_PY get_bdevs | jq '.[] | select(.name=="Nvme0n1")')" ]; then
		error "Nvme0n1 bdev not found!"
	fi

	$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1.0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1p0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 1 Nvme0n1p1
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 2 Nvme0n1p2
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 3 Nvme0n1p3
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 4 Nvme0n1p4
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 5 Nvme0n1p5

	$RPC_PY construct_malloc_bdev 128 512 --name Malloc0
	$RPC_PY construct_vhost_scsi_controller naa.Malloc0.1
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.1 0 Malloc0

	$RPC_PY construct_malloc_bdev 128 4096 --name Malloc1
	$RPC_PY construct_vhost_scsi_controller naa.Malloc1.2
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.2 0 Malloc1

	vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
	virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
}

timing_enter spdk_vhost_run
spdk_vhost_run $BASE_DIR
timing_exit spdk_vhost_run

timing_enter create_bdev_config
create_bdev_config
timing_exit create_bdev_config

timing_enter run_spdk_fio
run_spdk_fio $BASE_DIR/bdev.fio --filename=$virtio_bdevs --spdk_conf=$BASE_DIR/bdev.conf
timing_exit run_spdk_fio

rm -f *.state
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
