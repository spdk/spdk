#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

function run_spdk_fio() {
	fio_bdev --ioengine=spdk_bdev "$@" --spdk_mem=1024 --spdk_single_seg=1 \
		--verify_state_save=0
}

function err_cleanup() {
	killprocess $vhost_pid
}

function gen_vhost_conf() {
	local traddr dev_type vg_count name conf setup

	setup+=("naa.Nvme0n1_scsi0.0 scsi 8 VirtioScsi0")
	setup+=("naa.Nvme0n1_blk0.0  blk  8 VirtioBlk3")
	setup+=("naa.Nvme0n1_blk1.0  blk  8 VirtioBlk4")
	setup+=("naa.Malloc0.0       scsi 8 VirtioScsi1")
	setup+=("naa.Malloc1.0       scsi 8 VirtioScsi2")

	while read -r traddr dev_type vg_count name; do
		conf+=("$(
			cat <<- VHOST
				{
				  "method": "bdev_virtio_attach_controller",
				  "params": {
				    "name": "$name",
				    "dev_type": "$dev_type",
				    "trtype": "user",
				    "traddr": "$traddr",
				    "vq_count": $vg_count,
				    "vq_size": 512
				  }
				}
			VHOST
		)")
	done < <(printf '%s\n' "${setup[@]}")

	local IFS=","
	cat <<- VHOST
		{
		  "subsystems": [ {
		    "subsystem": "bdev",
		    "config": [
		      ${conf[*]}
		    ]
		  } ]
		}
	VHOST
}

# start vhost and configure it
trap 'err_cleanup; exit 1' SIGINT SIGTERM EXIT
$SPDK_BIN_DIR/vhost -m 0xf &
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
[[ "$(rpc_cmd vhost_get_controllers -n naa.Nvme0n1_scsi0.0 | jq -r '.[].cpumask')" == "0xf" ]]

rpc_cmd vhost_create_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p4 --cpumask 0xf
[[ "$(rpc_cmd vhost_get_controllers -n naa.Nvme0n1_blk0.0 | jq -r '.[].cpumask')" == "0xf" ]]
rpc_cmd vhost_create_blk_controller naa.Nvme0n1_blk1.0 Nvme0n1p5 --cpumask 0x1
[[ "$(rpc_cmd vhost_get_controllers -n naa.Nvme0n1_blk1.0 | jq -r '.[].cpumask')" == "0x1" ]]

rpc_cmd bdev_malloc_create 128 512 --name Malloc0
rpc_cmd vhost_create_scsi_controller naa.Malloc0.0 --cpumask 0x2
rpc_cmd vhost_scsi_controller_add_target naa.Malloc0.0 0 Malloc0
[[ "$(rpc_cmd vhost_get_controllers -n naa.Malloc0.0 | jq -r '.[].cpumask')" == "0x2" ]]

rpc_cmd bdev_malloc_create 128 4096 --name Malloc1
rpc_cmd vhost_create_scsi_controller naa.Malloc1.0 --cpumask 0xc
rpc_cmd vhost_scsi_controller_add_target naa.Malloc1.0 0 Malloc1
[[ "$(rpc_cmd vhost_get_controllers -n naa.Malloc1.0 | jq -r '.[].cpumask')" == "0xc" ]]

timing_enter run_spdk_fio
run_spdk_fio $testdir/bdev.fio --filename=* --section=job_randwrite --spdk_json_conf=<(gen_vhost_conf)
timing_exit run_spdk_fio

timing_enter run_spdk_fio_unmap
run_spdk_fio $testdir/bdev.fio --filename="VirtioScsi1t0:VirtioScsi2t0" --spdk_json_conf=<(gen_vhost_conf)
timing_exit run_spdk_fio_unmap

rpc_cmd bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT

killprocess $vhost_pid
