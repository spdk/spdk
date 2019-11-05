#!/usr/bin/env bash

set -e
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

vhosttestinit

#TODO: Both scsi and blk?

timing_enter vhost_fio

trap "at_app_exit; process_shm --id 0; exit 1" SIGINT SIGTERM EXIT

vhost_run vhost0 "-m 0x1"

# Create vhost scsi controller
vhost_rpc vhost0 bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
vhost_rpc vhost0 vhost_create_scsi_controller naa.VhostScsi0.0
vhost_rpc vhost0 vhost_scsi_controller_add_target naa.VhostScsi0.0 0 "Malloc0"

# Create vhost blk controller
vhost_rpc vhost0 bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1
vhost_rpc vhost0 vhost_create_blk_controller naa.Malloc1.1 Malloc1

# Start qemu based VMs
vm_setup --os="$VM_IMAGE" --disk-type=spdk_vhost_scsi --disks="VhostScsi0" --vhost-name=vhost0 --force=0
vm_setup --os="$VM_IMAGE" --disk-type=spdk_vhost_blk --disks="Malloc1" --vhost-name=vhost0 --force=1

vm_run 0
vm_run 1

vm_wait_for_boot 300 0
vm_wait_for_boot 300 1
sleep 5

# Run the fio workload on the VM
vm_scp 0 $testdir/vhost_fio.job 127.0.0.1:/root/vhost_fio.job
vm_exec 0 "fio /root/vhost_fio.job"

vm_scp 1 $testdir/vhost_fio.job 127.0.0.1:/root/vhost_fio.job
vm_exec 1 "fio /root/vhost_fio.job"

# Shut the VM down
vm_shutdown_all

# Shut vhost down
vhost_kill vhost0

trap - SIGINT SIGTERM EXIT

vhosttestfini
timing_exit vhost_fio
