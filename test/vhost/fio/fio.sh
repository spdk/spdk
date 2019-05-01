#!/usr/bin/env bash

set -e
testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

if [ ! -d $QEMU_PREFIX ]; then
        echo "qemu not installed on this machine. It may be a VM. Skipping vhost fio test."
        exit 0
fi

# pass the parameter 'iso' to this script when running it in isolation to trigger local initialization
vhosttestinit $1

#TODO: Both scsi and blk?

timing_enter vhost_fio
mkdir -p "$(get_vhost_dir 3)"
$VHOST_APP -S "$(get_vhost_dir 3)" &
vhostpid=$!
waitforlisten $vhostpid

trap "process_shm --id 0 killprocess $vhostpid; exit 1" SIGINT SIGTERM EXIT

# Construct vhost scsi controller
$VHOST_RPC construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$VHOST_RPC construct_vhost_scsi_controller naa.VhostScsi0.3
$VHOST_RPC add_vhost_scsi_lun naa.VhostScsi0.3 0 "Malloc0"

# Start qemu based VM.
vm_setup --os="$VHOST_VM_IMAGE" --disk-type=spdk_vhost_scsi --disks="VhostScsi0"  --force=3 --vhost-num=3
vm_run 3
vm_wait_for_boot 300 3
sleep 5

# Run the fio workload on the VM
vm_scp 3 $testdir/vhost_fio.job 127.0.0.1:/root/vhost_fio.job
vm_ssh "fio /root/vhost_fio.job"

# Shut the VM down
vm_shutdown_all

trap - SIGINT SIGTERM EXIT

killprocess $vhostpid
vhosttestfini $1
timing_exit vhost_fio