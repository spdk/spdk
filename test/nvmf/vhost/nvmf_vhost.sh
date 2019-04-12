#!/usr/bin/env bash

set -e
testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/vhost/common/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
NVMF_SOCK="/tmp/nvmf_rpc.sock"
NVMF_APP="$rootdir/app/nvmf_tgt/nvmf_tgt -r $NVMF_SOCK -i 0 -e 0xF"
NVMF_RPC="$rootdir/scripts/rpc.py -s $NVMF_SOCK"

VHOST_SOCK="/tmp/vhost_rpc.sock"
VHOST_APP="$rootdir/app/vhost/vhost -p 0 -r $VHOST_SOCK -u"
VHOST_RPC="$rootdir/scripts/rpc.py -s $VHOST_SOCK"
vm_image="/home/sys_sgsw/vhost_vm_image.qcow2"

DEFAULT_FIO_BIN="/home/sys_sgsw/fio_ubuntu"

if [ ! -d ]; then
echo "qemu not installed on this machine. It may be a VM. Skipping nvmf_vhost test."
exit 0
fi

nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter vhost_nvmf
# Start Apps
$NVMF_APP &
nvmfpid=$!
waitforlisten $nvmfpid $NVMF_SOCK

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

mkdir -p "$(get_vhost_dir 3)"

$VHOST_APP -S "$(get_vhost_dir 3)" &
vhostpid=$!
waitforlisten $vhostpid $NVMF_SOCK

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; killprocess $vhostpid nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

# Configure NVMF tgt on host machine
malloc_bdev="$($NVMF_RPC construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

$NVMF_RPC nvmf_create_transport -t RDMA -u 8192 -p 4
$NVMF_RPC nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$NVMF_RPC nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$malloc_bdev"
$NVMF_RPC nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Configure VHost on host machine
vhost_nvme_bdev="$($VHOST_RPC construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1)"
$VHOST_RPC construct_vhost_scsi_controller naa.VhostScsi0.3
$VHOST_RPC add_vhost_scsi_lun naa.VhostScsi0.3 0 "$vhost_nvme_bdev"
vhost_scsi_path="VhostScsi0"

# start qemu based VM.
vm_setup --os="$vm_image" --disk-type=spdk_vhost_scsi --disks="$vhost_scsi_path"  --force=3 --vhost-num=3

vm_run 3

vm_wait_for_boot 300 3

vm_start_fio_server --fio-bin=$DEFAULT_FIO_BIN 3

sleep 5

# Run the fio workload remotely
vm_scp 3 $testdir/nvmf_vhost_fio.job 127.0.0.1:/root/nvmf_vhost_fio.job
$DEFAULT_FIO_BIN --client=127.0.0.1,$(vm_fio_socket 3) --remote-config /root/nvmf_vhost_fio.job

vm_shutdown 3
nvmfcleanup
killprocess $vhostpid
killprocess $nvmfpid
nvmftestfini $1
timing_exit vhost_nvmf
