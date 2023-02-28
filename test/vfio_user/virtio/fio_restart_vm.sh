#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh
source $rootdir/test/vfio_user/virtio/common.sh
source $rootdir/test/vfio_user/autotest.config

bdfs=($(get_nvme_bdfs))
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

virtio_type=$1
if [[ $virtio_type != virtio_blk ]] && [[ $virtio_type != virtio_scsi ]]; then
	echo "Unsupported device type"
	exit 1
fi

function get_disks() {
	if [[ "$1" == "virtio_scsi" ]]; then
		vm_check_scsi_location $2
	elif [[ "$1" == "virtio_blk" ]]; then
		vm_check_blk_location $2
	fi
}

vhosttestinit

vfu_tgt_run 0

vfu_vm_dir="$VM_DIR/vfu_tgt"
rm -rf $vfu_vm_dir
mkdir -p $vfu_vm_dir

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t pcie -a ${bdfs[0]}

# using socket $VM_DIR/vfu_tgt/virtio.$disk_no
disk_no="1"
vm_num="1"
$rpc_py vfu_tgt_set_base_path $vfu_vm_dir

if [[ "$virtio_type" == "virtio_blk" ]]; then
	$rpc_py vfu_virtio_create_blk_endpoint virtio.$disk_no --bdev-name Nvme0n1 --num-queues=2 --qsize=512 --packed-ring
elif [[ "$virtio_type" == "virtio_scsi" ]]; then
	$rpc_py vfu_virtio_create_scsi_endpoint virtio.$disk_no --num-io-queues=2 --qsize=512 --packed-ring
	$rpc_py vfu_virtio_scsi_add_target virtio.$disk_no --scsi-target-num=0 --bdev-name Nvme0n1
fi

vm_setup --disk-type=vfio_user_virtio --force=1 --os=$VM_IMAGE --disks="1"
vm_run $vm_num
vm_wait_for_boot 60 $vm_num

# Get disk names from VM1 and run FIO traffic
fio_bin="--fio-bin=$FIO_BIN"
fio_disks=""
qemu_mask_param="VM_${vm_num}_qemu_mask"

host_name="VM-$vm_num-${!qemu_mask_param}"
vm_exec $vm_num "hostname $host_name"
vm_start_fio_server $fio_bin $vm_num

disks_before_restart=""
get_disks $virtio_type $vm_num
disks_before_restart="$SCSI_DISK"

fio_disks=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
job_file="default_integrity.job"

# Run FIO traffic
run_fio $fio_bin --job-file=$rootdir/test/vhost/common/fio_jobs/$job_file --out="$VHOST_DIR/fio_results" $fio_disks

# execute "poweroff" for vm 1
notice "Shutting down virtual machine..."
vm_shutdown_all

# re-launch the vm
vm_setup --disk-type=vfio_user_virtio --force=1 --os=$VM_IMAGE --disks="1"
vm_run $vm_num
vm_wait_for_boot 60 $vm_num

# compare block device with VM before restart
disks_after_restart=""
get_disks $virtio_type $vm_num
disks_after_restart="$SCSI_DISK"

if [[ "$disks_after_restart" != "$disks_before_restart" ]]; then
	error "Disks aren't same after restart"
	exit 1
fi

# execute "poweroff" for vm 1
notice "Shutting down virtual machine..."
vm_shutdown_all

$rpc_py bdev_nvme_detach_controller Nvme0

vhost_kill 0

vhosttestfini
