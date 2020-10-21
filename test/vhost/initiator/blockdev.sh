#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

function run_spdk_fio() {
	fio_bdev --ioengine=spdk_bdev "$@" --spdk_mem=1024 --spdk_single_seg=1 \
		--verify_state_save=0
}

function err_cleanup() {
	rm -f $testdir/bdev.json
	killprocess $vhost_pid
	if [[ -n "$dummy_spdk_pid" ]] && kill -0 $dummy_spdk_pid &> /dev/null; then
		killprocess $dummy_spdk_pid
	fi
}

# start vhost and configure it
trap 'err_cleanup; exit 1' SIGINT SIGTERM EXIT
$SPDK_BIN_DIR/vhost &
vhost_pid=$!
waitforlisten $vhost_pid

$rootdir/scripts/gen_nvme.sh | $rootdir/scripts/rpc.py load_subsystem_config
if [ -z "$(rpc_cmd bdev_get_bdevs | jq '.[] | select(.name=="Nvme0n1")')" ]; then
	echo "Nvme0n1 bdev not found!" && false
fi

rpc_cmd bdev_split_create Nvme0n1 6

rpc_cmd vhost_create_scsi_controller naa.Nvme0n1_scsi0.0
rpc_cmd vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
rpc_cmd vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 1 Nvme0n1p1
rpc_cmd vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 2 Nvme0n1p2
rpc_cmd vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 3 Nvme0n1p3

rpc_cmd vhost_create_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p4
rpc_cmd vhost_create_blk_controller naa.Nvme0n1_blk1.0 Nvme0n1p5

rpc_cmd bdev_malloc_create 128 512 --name Malloc0
rpc_cmd vhost_create_scsi_controller naa.Malloc0.0
rpc_cmd vhost_scsi_controller_add_target naa.Malloc0.0 0 Malloc0

rpc_cmd bdev_malloc_create 128 4096 --name Malloc1
rpc_cmd vhost_create_scsi_controller naa.Malloc1.0
rpc_cmd vhost_scsi_controller_add_target naa.Malloc1.0 0 Malloc1

# start a dummy app, create vhost bdevs in it, then dump the config for FIO
$SPDK_BIN_DIR/spdk_tgt -r /tmp/spdk2.sock -g &
dummy_spdk_pid=$!
waitforlisten $dummy_spdk_pid /tmp/spdk2.sock
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_scsi0.0' -d scsi --vq-count 8 'VirtioScsi0'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_blk0.0' -d blk --vq-count 8 'VirtioBlk3'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Nvme0n1_blk1.0' -d blk --vq-count 8 'VirtioBlk4'

rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Malloc0.0' -d scsi --vq-count 8 'VirtioScsi1'
rpc_cmd -s /tmp/spdk2.sock bdev_virtio_attach_controller --trtype user --traddr 'naa.Malloc1.0' -d scsi --vq-count 8 'VirtioScsi2'

cat <<- CONF > $testdir/bdev.json
	{"subsystems":[
	$(rpc_cmd -s /tmp/spdk2.sock save_subsystem_config -n bdev)
	]}
CONF
killprocess $dummy_spdk_pid

# run FIO with previously acquired spdk config files
timing_enter run_spdk_fio
run_spdk_fio $testdir/bdev.fio --filename=* --section=job_randwrite --spdk_json_conf=$testdir/bdev.json
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
run_spdk_fio $testdir/bdev.fio --filename="VirtioScsi1t0:VirtioScsi2t0" --spdk_json_conf=$testdir/bdev.json
timing_exit run_spdk_fio_unmap

rpc_cmd bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT
rm -f $testdir/bdev.json

killprocess $vhost_pid
