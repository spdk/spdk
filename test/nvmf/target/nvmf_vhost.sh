#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ ! -d "/usr/local/qemu/spdk-3.0.0" ]; then
	echo "Qemu not installed on this machine. It may be a VM. Skipping nvmf_vhost test."
	exit 0
fi

source $rootdir/test/vhost/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
NVMF_SOCK="/tmp/nvmf_rpc.sock"
NVMF_RPC="$rootdir/scripts/rpc.py -s $NVMF_SOCK"

VHOST_SOCK="/tmp/vhost_rpc.sock"
VHOST_APP+=(-p 0 -r "$VHOST_SOCK" -u)
VHOST_RPC="$rootdir/scripts/rpc.py -s $VHOST_SOCK"

nvmftestinit

# Start Apps
"${NVMF_APP[@]}" -r $NVMF_SOCK &
nvmfpid=$!
waitforlisten $nvmfpid $NVMF_SOCK

trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

mkdir -p "$(get_vhost_dir 3)"

"${VHOST_APP[@]}" -S "$(get_vhost_dir 3)" &
vhostpid=$!
waitforlisten $vhostpid $NVMF_SOCK

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $vhostpid nvmftestfini; exit 1' SIGINT SIGTERM EXIT

# Configure NVMF tgt on host machine
malloc_bdev="$($NVMF_RPC bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

$NVMF_RPC nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -p 4
$NVMF_RPC nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$NVMF_RPC nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$malloc_bdev"
$NVMF_RPC nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Configure VHost on host machine
$VHOST_RPC bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -f ipv4 -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
$VHOST_RPC vhost_create_scsi_controller naa.VhostScsi0.3
$VHOST_RPC vhost_scsi_controller_add_target naa.VhostScsi0.3 0 "Nvme0n1"

# start qemu based VM.
vm_setup --os="$VM_IMAGE" --disk-type=spdk_vhost_scsi --disks="VhostScsi0" --force=3 --vhost-name=3

vm_run 3

vm_wait_for_boot 300 3

# Run the fio workload remotely
vm_scp 3 $testdir/nvmf_vhost_fio.job 127.0.0.1:/root/nvmf_vhost_fio.job
vm_exec 3 "fio /root/nvmf_vhost_fio.job"
vm_shutdown_all

trap - SIGINT SIGTERM EXIT

killprocess $vhostpid
nvmftestfini
