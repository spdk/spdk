#!/usr/bin/env bash

set -e
INITIATOR_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $INITIATOR_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $INITIATOR_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
FIO_PATH="/usr/src/fio"
virtio_bdevs=""
virtio_with_unmap=""
os_image="/home/sys_sgsw/vhost_vm_image.qcow2"
#different linux distributions have different versions of targetcli that have different names for ramdisk option
targetcli_rd_name=""
kernel_vhost_disk="naa.5012345678901234"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help            Print help and exit"
	echo "    --vm_image=PATH   Path to VM image used in these tests [default=$os_image]"
	echo "    --fiopath=PATH    Path to fio directory on host [default=$FIO_PATH]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiopath=*) FIO_PATH="${OPTARG#*=}" ;;
			vm_image=*) os_image="${OPTARG#*=}" ;;
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

if targetcli ls backstores | grep ramdisk ; then
	targetcli_rd_name="ramdisk"
elif targetcli ls backstores | grep rd_mcp ; then
	targetcli_rd_name="rd_mcp"
else
	error "targetcli: cannot create a ramdisk.\
	 Neither backstores/ramdisk nor backstores/rd_mcp is available"
fi

ramdisk_flag=false
vhost_disk_flag=false

function remove_kernel_vhost()
{
	if $vhost_disk_flag; then
		targetcli "/vhost delete $kernel_vhost_disk"
	fi
	if $ramdisk_flag; then
		targetcli "/backstores/$targetcli_rd_name delete ramdisk"
	fi
}

trap 'rm -f *.state $ROOT_DIR/spdk.tar.gz $ROOT_DIR/fio.tar.gz $(get_vhost_dir)/Virtio0;\
 remove_kernel_vhost; error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT
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

timing_enter create_kernel_vhost
targetcli "/backstores/$targetcli_rd_name create name=ramdisk size=1GB"
ramdisk_flag=true
targetcli "/vhost create $kernel_vhost_disk"
vhost_disk_flag=true
targetcli "/vhost/$kernel_vhost_disk/tpg1/luns create /backstores/$targetcli_rd_name/ramdisk"
timing_exit create_kernel_vhost

timing_enter setup_vm
vm_no="0"
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$os_image \
 --disks="Nvme0n1_scsi0:Malloc0:Malloc1:$kernel_vhost_disk,kernel_vhost:Virtio0,virtio:\
 Nvme0n1_blk0,spdk_vhost_blk:Nvme0n1_blk1,spdk_vhost_blk" \
 --queue_num=8 --memory=6144
vm_run $vm_no

timing_enter vm_wait_for_boot
vm_wait_for_boot 300 $vm_no
timing_exit vm_wait_for_boot

timing_enter vm_scp_spdk
touch $ROOT_DIR/spdk.tar.gz
tar --exclude="spdk.tar.gz" --exclude="*.o" --exclude="*.d" --exclude=".git" -C $ROOT_DIR -zcf $ROOT_DIR/spdk.tar.gz .
vm_scp $vm_no $ROOT_DIR/spdk.tar.gz "127.0.0.1:/root"
vm_ssh $vm_no "mkdir -p /root/spdk; tar -zxf /root/spdk.tar.gz -C /root/spdk --strip-components=1"

touch $ROOT_DIR/fio.tar.gz
tar --exclude="fio.tar.gz" --exclude="*.o" --exclude="*.d" --exclude=".git" -C $FIO_PATH -zcf $ROOT_DIR/fio.tar.gz .
vm_scp $vm_no $ROOT_DIR/fio.tar.gz "127.0.0.1:/root"
vm_ssh $vm_no "rm -rf /root/fio_src; mkdir -p /root/fio_src; tar -zxf /root/fio.tar.gz -C /root/fio_src --strip-components=1"
timing_exit vm_scp_spdk

timing_enter vm_build_spdk
nproc=$(vm_ssh $vm_no "nproc")
vm_ssh $vm_no " cd /root/fio_src ; make clean ; make -j${nproc} ; make install"
vm_ssh $vm_no " cd spdk ; ./configure --with-fio=/root/fio_src ; make clean ; make -j${nproc}"
timing_exit vm_build_spdk

vm_ssh $vm_no "/root/spdk/scripts/setup.sh"
vbdevs=$(vm_ssh $vm_no ". /root/spdk/test/common/autotest_common.sh && discover_bdevs /root/spdk \
 /root/spdk/test/vhost/initiator/bdev_pci.conf")
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
virtio_with_unmap=$(jq -r '[.[] | select(.supported_io_types.unmap==true).name]
 | join(":")' <<< $vbdevs)
timing_exit setup_vm

timing_enter run_spdk_fio_pci
vm_ssh $vm_no "LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_bdevs --section=job_randwrite \
 --section=job_randrw --section=job_write --section=job_rw \
 --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024 --spdk_single_seg=1"
timing_exit run_spdk_fio_pci

timing_enter run_spdk_fio_pci_unmap
vm_ssh $vm_no "LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev \
 /root/spdk/test/vhost/initiator/bdev.fio --filename=$virtio_with_unmap \
 --spdk_conf=/root/spdk/test/vhost/initiator/bdev_pci.conf --spdk_mem=1024 --spdk_single_seg=1"
timing_exit run_spdk_fio_pci_unmap

timing_enter vm_shutdown_all
vm_shutdown_all
timing_exit vm_shutdown_all

rm -f *.state $ROOT_DIR/spdk.tar.gz $ROOT_DIR/fio.tar.gz $(get_vhost_dir)/Virtio0
timing_enter remove_kernel_vhost
remove_kernel_vhost
timing_exit remove_kernel_vhost

$RPC_PY delete_nvme_controller Nvme0

timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
