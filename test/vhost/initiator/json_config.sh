#!/usr/bin/env bash
set -ex
INITIATOR_JSON_DIR=$(readlink -f $(dirname $0))
. $INITIATOR_JSON_DIR/../../json_config/common.sh

# Load spdk_tgt with controllers used by virtio initiator
# Test also virtio_pci bdevs
function pre_initiator_config() {
	$rpc_py construct_split_vbdev Nvme0n1 4
	$rpc_py construct_vhost_scsi_controller $JSON_DIR/naa.Nvme0n1p0.0
	$rpc_py construct_vhost_scsi_controller $JSON_DIR/naa.Nvme0n1p1.1
	$rpc_py add_vhost_scsi_lun $JSON_DIR/naa.Nvme0n1p0.0 0 Nvme0n1p0
	$rpc_py add_vhost_scsi_lun $JSON_DIR/naa.Nvme0n1p1.1 0 Nvme0n1p1
	$rpc_py construct_vhost_blk_controller $JSON_DIR/naa.Nvme0n1p2.0 Nvme0n1p2
	$rpc_py construct_vhost_blk_controller $JSON_DIR/naa.Nvme0n1p3.1 Nvme0n1p3
	pci_scsi=$(lspci -nn -D | grep '1af4:1004' | head -1 | awk '{print $1;}')
	pci_blk=$(lspci -nn -D | grep '1af4:1001' | head -1 | awk '{print $1;}')
	if [ ! -z $pci_scsi ]; then
		$rpc_py construct_virtio_pci_scsi_bdev $pci_scsi Virtio0
	fi
	if [ ! -z $pci_blk ]; then
		$rpc_py construct_virtio_pci_blk_bdev $pci_blk Virtio1
	fi
}

# Load virtio initiator with bdevs
function upload_initiator() {
	$rpc_py construct_virtio_user_scsi_bdev $JSON_DIR/naa.Nvme0n1p0.0 Nvme0n1p0
	$rpc_py construct_virtio_user_blk_bdev $JSON_DIR/naa.Nvme0n1p2.0 Nvme0n1p2
}

function clean_upload_initiator() {
	$clear_config_py clear_config
}

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $INITIATOR_JSON_DIR/../../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	pre_initiator_config
	test_json_config
	run_initiator
	rpc_py="$initiator_rpc_py"
	clear_config_py="$initiator_clear_config_py"
	$rpc_py start_subsystem_init
	upload_initiator
	test_json_config
	clean_upload_initiator
        test_global_params "virtio_initiator"
	clear_config_py="$spdk_clear_config_py"
	$clear_config_py clear_config
	kill_targets

	rpc_py="$spdk_rpc_py"
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

test_subsystems
