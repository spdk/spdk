#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

vhosttestinit

source $testdir/autotest.config
RPC_PY="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

function run_spdk_fio() {
	fio_bdev --ioengine=spdk_bdev "$@" --spdk_mem=1024 --spdk_single_seg=1
}

function create_bdev_config()
{
	if [ -z "$($RPC_PY bdev_get_bdevs | jq '.[] | select(.name=="Nvme0n1")')" ]; then
		error "Nvme0n1 bdev not found!"
	fi

	$RPC_PY bdev_split_create Nvme0n1 6

	$RPC_PY vhost_create_scsi_controller naa.Nvme0n1_scsi0.0
	$RPC_PY vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
	$RPC_PY vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 1 Nvme0n1p1
	$RPC_PY vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 2 Nvme0n1p2
	$RPC_PY vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 3 Nvme0n1p3

	$RPC_PY vhost_create_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p4
	$RPC_PY vhost_create_blk_controller naa.Nvme0n1_blk1.0 Nvme0n1p5

	$RPC_PY bdev_malloc_create 128 512 --name Malloc0
	$RPC_PY vhost_create_scsi_controller naa.Malloc0.0
	$RPC_PY vhost_scsi_controller_add_target naa.Malloc0.0 0 Malloc0

	$RPC_PY bdev_malloc_create 128 4096 --name Malloc1
	$RPC_PY vhost_create_scsi_controller naa.Malloc1.0
	$RPC_PY vhost_scsi_controller_add_target naa.Malloc1.0 0 Malloc1
}

function err_cleanup() {
	rm -f $testdir/bdev.json
	vhost_kill 0
	if [[ -n "$dummy_spdk_pid" ]] && kill -0 $dummy_spdk_pid &>/dev/null; then
		killprocess $dummy_spdk_pid
	fi
	vhosttestfini
}

timing_enter vhost_run
vhost_run 0
timing_exit vhost_run

trap 'err_cleanup; exit 1' SIGINT SIGTERM EXIT

timing_enter create_bdev_config
create_bdev_config
timing_exit create_bdev_config

# start a dummy app and generate a json config for FIO
$rootdir/app/spdk_tgt/spdk_tgt -r /tmp/spdk2.sock -g &
dummy_spdk_pid=$!
waitforlisten $dummy_spdk_pid /tmp/spdk2.sock
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_scsi0.0' -d scsi --vq-count 8 'VirtioScsi0'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_blk0.0' -d blk --vq-count 8 'VirtioBlk3'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_blk1.0' -d blk --vq-count 8 'VirtioBlk4'

rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Malloc0.0' -d scsi --vq-count 8 'VirtioScsi1'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Malloc1.0' -d scsi --vq-count 8 'VirtioScsi2'

cat <<-CONF > $testdir/bdev.json
	{"subsystems":[
	$(rpc_cmd -s /tmp/spdk2.sock save_subsystem_config -n bdev)
	]}
CONF
killprocess $dummy_spdk_pid

timing_enter run_spdk_fio
run_spdk_fio $testdir/bdev.fio --filename=* --section=job_randwrite --section=job_randrw \
	--section=job_write --section=job_rw --spdk_json_conf=$testdir/bdev.json
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
run_spdk_fio $testdir/bdev.fio --filename="VirtioScsi1t0:VirtioScsi2t0" --spdk_json_conf=$testdir/bdev.json
timing_exit run_spdk_fio_unmap

$RPC_PY bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT
rm -f $testdir/bdev.json

timing_enter vhost_kill
vhost_kill 0
timing_exit vhost_kill

vhosttestfini
