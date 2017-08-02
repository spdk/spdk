#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

. $BASE_DIR/common.sh

function gen_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 16
END_OF_CONFIG
}

function pre_test_case() {
    used_vms=""
    run_vhost
    rm $BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p1.0
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p2.1
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p3.1
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p4.2
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p5.2
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p6.3
    $SPDK_BUILD_DIR/scripts/rpc.py construct_vhost_scsi_controller naa.Nvme0n1p7.3
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p4.2 0 Nvme0n1p8
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p4.2 1 Nvme0n1p9
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p5.2 0 Nvme0n1p10
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p5.2 1 Nvme0n1p11
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p6.3 0 Nvme0n1p12
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p6.3 1 Nvme0n1p13
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p7.3 0 Nvme0n1p14
    $SPDK_BUILD_DIR/scripts/rpc.py add_vhost_scsi_lun naa.Nvme0n1p7.3 1 Nvme0n1p15
    vms_setup_and_run "0 1 2 3"
    vms_prepare "0 1 2 3"
}

function reboot_all_and_prepare() {
    vms_reboot_all $1
    vms_prepare $1
}

function post_test_case() {
    vm_shutdown_all
    spdk_vhost_kill
}

gen_config
pre_test_case
$BASE_DIR/scsi_hotattach.sh --fio-bin=$fio_bin &
first_script=$!
$BASE_DIR/scsi_hotdetach.sh --fio-bin=$fio_bin &
second_script=$!
{
    w="a"
    set +e
    while [ ! -z "$w" ]; do
        echo "vhost is alive"
        w="$( ps ax | grep -v grep | grep app/vhost || true )"
        sleep 2
    done
    set -e
    echo "vhost is dead"
} &
wait $first_script
wait $second_script
post_test_case
