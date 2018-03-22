#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))

. $BASE_DIR/../common/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

function run_vhost() {
    notice "==============="
    notice ""
    notice "running SPDK"
    notice ""
    spdk_vhost_run --conf-path=$BASE_DIR
    notice ""
}

# Add split section into vhost config
function gen_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 2
  Split Nvme1n1 2
END_OF_CONFIG
}

function test_rpc_subsystem() {
    $rpc_py get_subsystems
    $rpc_py get_subsystem_config copy
    $rpc_py get_subsystem_config interface
    $rpc_py get_subsystem_config net_framework
    $rpc_py get_subsystem_config bdev
    $rpc_py get_subsystem_config nbd
    $rpc_py get_subsystem_config scsi
    $rpc_py get_subsystem_config vhost
}

function test_json_config() {
    $rpc_py save_config
    $rpc_py save_config -f $BASE_DIR/sample.json
    $rpc_py load_config --filename $BASE_DIR/sample.json
    $rpc_py save_config -f $BASE_DIR/sample_2.json
    diff $BASE_DIR/sample.json $BASE_DIR/sample_2.json
    rm sample.json sample_2.json || true
}

function upload_vhost() {
    . $BASE_DIR/../hotplug/common.sh
    traddr=""
    get_traddr "Nvme0"
    $rpc_py construct_nvme_bdev -b "Nvme0" -t "PCIe" -a $traddr
    traddr=""
    get_traddr "Nvme1"
    $rpc_py construct_nvme_bdev -b "Nvme1" -t "PCIe" -a $traddr
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.1 1 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p0.0 Nvme0n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p1.1 Nvme1n1p1

    $rpc_py construct_malloc_bdev 128 512 --name Malloc0
    $rpc_py construct_malloc_bdev 64 4096 --name Malloc1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 1 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Malloc1.0 Malloc1

    $rpc_py construct_lvol_store Malloc0 lvs_test 1048576
    $rpc_py construct_lvol_bdev lvs_test lvol0 32
    $rpc_py start_nbd_disk lvol0 /dev/nbd0
    $rpc_py snapshot_lvol_bdev lvol0 snap0
    $rpc_py clone_lvol_bdev snap0 clone0
}

function test_vhost() {
    run_vhost

    test_rpc_subsystem
    test_json_config

    spdk_vhost_kill
}

function test_nvmf() {
    . $BASE_DIR/../../nvmf/common.sh
    echo ""
}

function test_iscsi() {
    echo ""
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
gen_config
modprobe nbd

test_vhost
test_nvmf
test_iscsi

rmmod nbd
