#!/usr/bin/env bash

set -e
INITIATOR_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $INITIATOR_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $INITIATOR_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
FIO_PATH="/usr/src/fio"
virtio_bdevs=""
virtio_with_unmap=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help            Print help and exit"
	echo "    --fiopath=PATH    Path to fio directory on host [default=$FIO_PATH]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiopath=*) FIO_PATH="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $COMMON_DIR/common.sh
source $INITIATOR_DIR/autotest.config
PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

if [ ! -x $FIO_PATH ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

trap 'rm -f *.state $ROOT_DIR/spdk.tar.gz $ROOT_DIR/fio.tar.gz $(get_vhost_dir)/Virtio0;\
 error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT
function run_spdk_fio() {
	LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_PATH/fio --ioengine=spdk_bdev\
         "$@" --spdk_mem=1024 --spdk_single_seg=1
}

function create_bdev_config()
{
	local vbdevs

	if [ -z "$($RPC_PY get_bdevs | jq '.[] | select(.name=="Nvme0n1")')" ]; then
		error "Nvme0n1 bdev not found!"
	fi

	$RPC_PY construct_split_vbdev Nvme0n1 6

	$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1_scsi0.0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1_scsi0.0 1 Nvme0n1p1
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1_scsi0.0 2 Nvme0n1p2
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1_scsi0.0 3 Nvme0n1p3

	$RPC_PY construct_vhost_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p4
	$RPC_PY construct_vhost_blk_controller naa.Nvme0n1_blk1.0 Nvme0n1p5

	$RPC_PY construct_malloc_bdev 128 512 --name Malloc0
	$RPC_PY construct_vhost_scsi_controller naa.Malloc0.0
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.0 0 Malloc0

	$RPC_PY construct_malloc_bdev 128 4096 --name Malloc1
	$RPC_PY construct_vhost_scsi_controller naa.Malloc1.0
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.0 0 Malloc1

	vbdevs=$(discover_bdevs $ROOT_DIR $INITIATOR_DIR/bdev.conf)
	virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
	virtio_with_unmap=$(jq -r '[.[] | select(.supported_io_types.unmap==true).name]
	 | join(":")' <<< $vbdevs)
}

timing_enter spdk_vhost_run
spdk_vhost_run
timing_exit spdk_vhost_run

timing_enter create_bdev_config
create_bdev_config
timing_exit create_bdev_config

timing_enter run_spdk_fio
run_spdk_fio $INITIATOR_DIR/bdev.fio --filename=$virtio_bdevs --section=job_randwrite --section=job_randrw \
	--section=job_write --section=job_rw --spdk_conf=$INITIATOR_DIR/bdev.conf
report_test_completion "vhost_run_spdk_fio"
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
run_spdk_fio $INITIATOR_DIR/bdev.fio --filename=$virtio_with_unmap --spdk_conf=$INITIATOR_DIR/bdev.conf \
	--spdk_conf=$INITIATOR_DIR/bdev.conf
timing_exit run_spdk_fio_unmap

$RPC_PY delete_nvme_controller Nvme0

timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
