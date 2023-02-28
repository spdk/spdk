#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation. All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"

vfu_dir="/tmp/vfu_devices"
rm -rf $vfu_dir
mkdir -p $vfu_dir

# Start `spdk_tgt` and configure it
$SPDK_BIN_DIR/spdk_tgt -m 0xf -L vfu_virtio &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

$rpc_py bdev_malloc_create -b malloc0 64 512
$rpc_py bdev_malloc_create -b malloc1 64 512
$rpc_py bdev_malloc_create -b malloc2 64 512

$rpc_py vfu_tgt_set_base_path $vfu_dir

# Create vfio-user virtio-blk device
$rpc_py vfu_virtio_create_blk_endpoint vfu.blk --bdev-name malloc0 --cpumask=0x1 \
	--num-queues=2 --qsize=256 --packed-ring

# Create vfio-user virtio-scsi device
$rpc_py vfu_virtio_create_scsi_endpoint vfu.scsi --cpumask 0x2 --num-io-queues=2 \
	--qsize=256 --packed-ring
$rpc_py vfu_virtio_scsi_add_target vfu.scsi --scsi-target-num=0 --bdev-name malloc1
$rpc_py vfu_virtio_scsi_add_target vfu.scsi --scsi-target-num=1 --bdev-name malloc2

# Start bdevperf
bdevperf=$rootdir/build/examples/bdevperf
bdevperf_rpc_sock=/tmp/bdevperf.sock

$bdevperf -r $bdevperf_rpc_sock -g -s 2048 -q 256 -o 4096 -w randrw -M 50 -t 30 -m 0xf0 -L vfio_pci -L virtio_vfio_user &
bdevperf_pid=$!
trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock
$rpc_py -s $bdevperf_rpc_sock bdev_virtio_attach_controller --dev-type scsi \
	--trtype vfio-user --traddr $vfu_dir/vfu.scsi VirtioScsi0
$rpc_py -s $bdevperf_rpc_sock bdev_virtio_attach_controller --dev-type blk \
	--trtype vfio-user --traddr $vfu_dir/vfu.blk VirtioBlk0

# Start the tests
$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests

killprocess $bdevperf_pid
trap - SIGINT SIGTERM EXIT

# Delete the endpoints
$rpc_py vfu_virtio_delete_endpoint vfu.blk
$rpc_py vfu_virtio_delete_endpoint vfu.scsi

killprocess $spdk_tgt_pid
