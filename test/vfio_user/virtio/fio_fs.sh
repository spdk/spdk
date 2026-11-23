#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh
source $rootdir/test/vfio_user/virtio/common.sh
source $rootdir/test/vfio_user/autotest.config

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

vhosttestinit

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vfu_tgt_run 0 -m 0xf -s 512

vfu_vm_dir="$VM_DIR/vfu_tgt"
rm -rf $vfu_vm_dir
mkdir -p $vfu_vm_dir

# using socket $VM_DIR/vfu_tgt/virtio.$disk_no
disk_no="1"
vm_num="1"
job_file="default_fsdev.job"
be_virtiofs_dir="$SPDK_TEST_STORAGE/vfio-test.$disk_no"
vm_virtiofs_dir="/tmp/virtiofs.$disk_no"

$rpc_py vfu_tgt_set_base_path $vfu_vm_dir

rm -rf $be_virtiofs_dir
mkdir -p $be_virtiofs_dir

# we'll use this file to make sure that the mount succeeded
tmpfile=$(mktemp --tmpdir=$be_virtiofs_dir)

$rpc_py fsdev_aio_create aio.$disk_no $be_virtiofs_dir
$rpc_py vfu_virtio_create_fs_endpoint virtio.$disk_no --fsdev-name aio.$disk_no \
	--tag vfu_test.$disk_no --num-queues=2 --qsize=512 --packed-ring

vm_setup --disk-type=vfio_user_virtio --force=1 --os=$VM_IMAGE --disks="1"
vm_run $vm_num
vm_wait_for_boot 60 $vm_num

vm_exec $vm_num "mkdir $vm_virtiofs_dir"
vm_exec $vm_num "mount -t virtiofs vfu_test.$disk_no $vm_virtiofs_dir"
# if mount succeeded then the VM should see the file created by the host
vm_exec $vm_num "ls $vm_virtiofs_dir/$(basename $tmpfile)"
vm_start_fio_server --fio-bin="$FIO_BIN" $vm_num
run_fio --fio-bin="$FIO_BIN" --job-file=$rootdir/test/vhost/common/fio_jobs/$job_file --out="$VHOST_DIR/fio_results" --vm="$vm_num:$vm_virtiofs_dir/test"
vm_exec $vm_num "umount $vm_virtiofs_dir"

# execute "poweroff" for vm 1
notice "Shutting down virtual machine..."
vm_shutdown_all

vhost_kill 0

vhosttestfini
