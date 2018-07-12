#!/usr/bin/env bash
set -ex
INITIATOR_JSON_DIR=$(readlink -f $(dirname $0))
. $INITIATOR_JSON_DIR/../../json_config/common.sh

# Load spdk_tgt with controllers used by virtio initiator
# Test also virtio_pci bdevs
function construct_vhost_devices() {
	$rpc_py construct_split_vbdev Nvme0n1 4
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.1
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.1 0 Nvme0n1p1
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.0 Nvme0n1p2
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme0n1p3
	pci_scsi=$(lspci -nn -D | grep '1af4:1004' | head -1 | awk '{print $1;}')
	pci_blk=$(lspci -nn -D | grep '1af4:1001' | head -1 | awk '{print $1;}')
	if [ ! -z $pci_scsi ]; then
		$rpc_py construct_virtio_dev -t pci -a $pci_scsi -d scsi Virtio0
	fi
	if [ ! -z $pci_blk ]; then
		$rpc_py construct_virtio_dev -t pci -a $pci_blk -d blk Virtio1
	fi
}

# Load virtio initiator with bdevs
function connect_to_vhost_devices_from_initiator() {
	$rpc_py construct_virtio_dev -t user -a naa.Nvme0n1p0.0 -d scsi Nvme0n1p0
	$rpc_py construct_virtio_dev -t user -a naa.Nvme0n1p2.0 -d blk Nvme0n1p2
}

function disconnect_and_clear_vhost_devices() {
	$clear_config_py clear_config
}

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $INITIATOR_JSON_DIR/../../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	construct_vhost_devices
	test_json_config
	run_initiator
	rpc_py="$initiator_rpc_py"
	clear_config_py="$initiator_clear_config_py"
	$rpc_py start_subsystem_init
	connect_to_vhost_devices_from_initiator
	test_json_config
	disconnect_and_clear_vhost_devices
        test_global_params "virtio_initiator"
	clear_config_py="$spdk_clear_config_py"
	$clear_config_py clear_config
	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
timing_enter json_config_virtio_initiator

test_subsystems
timing_exit json_config_virtio_initiator
report_test_completion json_config_virtio_initiator
