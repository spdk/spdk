#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"
FIO_BIN="/usr/src/fio/fio"
BDEV_FIO="$BASE_DIR/bdev.fio"
virtio_bdevs=""
virtio_with_unmap=""
os_image="/home/sys_sgsw/vhost_vm_image.qcow2"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help            Print help and exit"
	echo "    --vm_image=PATH   Path to VM image used in these tests [default=/home/sys_sgsw/vhost_vm_image.qcow2]"
	echo "    --fiobin=PATH     Path to fio binary on host [default=/usr/src/fio/fio]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			vm_image=*) os_image="${OPTARG#*=}" ;;
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
	$RPC_PY construct_vhost_scsi_controller naa.Malloc0.0
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.0 0 Malloc0

	$RPC_PY construct_malloc_bdev 128 4096 --name Malloc1
	$RPC_PY construct_vhost_scsi_controller naa.Malloc1.0
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.0 0 Malloc1

	vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
	virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
	virtio_with_unmap=$(jq -r '[.[] | select(.supported_io_types.unmap==true).name]
	 | join(":")' <<< $vbdevs)
}

timing_enter spdk_vhost_run
spdk_vhost_run $BASE_DIR
timing_exit spdk_vhost_run

timing_enter create_bdev_config
create_bdev_config
timing_exit create_bdev_config

timing_enter run_spdk_fio
run_spdk_fio $BASE_DIR/bdev.fio --filename=$virtio_bdevs --section=job_randwrite --section=job_randrw \
	--section=job_write --section=job_rw --spdk_conf=$BASE_DIR/bdev.conf
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
run_spdk_fio $BASE_DIR/bdev.fio --filename=$virtio_with_unmap --section=job_randwrite \
	--section=job_randrw --section=job_unmap_trim_sequential --section=job_unmap_trim_random \
	--section=job_unmap_write --spdk_conf=$BASE_DIR/bdev.conf --spdk_conf=$BASE_DIR/bdev.conf
timing_exit run_spdk_fio_unmap

timing_enter run_spdk_fio_PCI
vm_no="0"
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$os_image --disks="Malloc0:Malloc1" --queue_num=12 --memory=6144
vm_run $vm_no
vm_wait_for_boot 600 $vm_no
vm_scp $vm_num -r $ROOT_DIR "127.0.0.1:/root/spdk"
vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make -j3"
vm_ssh $vm_num "/root/spdk/test/vhost/initiator/blockdev_pci.sh"
vm_shutdown_all
timing_exit run_spdk_fio_PCI

rm -f *.state
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
