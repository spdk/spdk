#!/usr/bin/env bash
set -xe

testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh

trap 'killprocess $virtio_pid; on_error_exit' ERR
timing_enter spdk_cli_vhost

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter run_spdk_virtio
$SPDKCLI_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x4 -p 0 -g -u -s 1024 -r /var/tmp/virtio.sock &
virtio_pid=$!
waitforlisten $virtio_pid /var/tmp/virtio.sock
timing_exit run_spdk_virtio

timing_enter spdkcli_create_virtio_pci_config
$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
pci_blk=$(lspci -nn -D | grep '1af4:1001' | head -1 | awk '{print $1;}')
if [ ! -z $pci_blk ]; then
	$spdkcli_job "/bdevs/virtioblk_disk create virtioblk_pci pci $pci_blk" "virtioblk_pci" True
fi
pci_scsi=$(lspci -nn -D | grep '1af4:1004' | head -1 | awk '{print $1;}')
if [ ! -z $pci_scsi ]; then
	$spdkcli_job "/bdevs/virtioscsi_disk create virtioscsi_pci pci $pci_scsi" "virtioscsi_pci" True
fi
$spdkcli_job "/vhost/scsi create sample_scsi" "sample_scsi" True
$spdkcli_job "/vhost/scsi/sample_scsi add_lun 0 Malloc0" "Malloc0" True
$spdkcli_job "/vhost/block create sample_block Malloc1" "Malloc1" True
timing_exit spdkcli_create_virtio_pci_config

timing_enter spdkcli_check_match
if [ ! -z $pci_blk ]  && [ ! -z $pci_scsi ]; then
        MATCH_FILE="spdkcli_virtio_pci.test"
        SPDKCLI_BRANCH="/bdevs"
        check_match
fi
timing_exit spdkcli_check_match

timing_exit spdkcli_create_virtio_user_config
$spdkcli_job "/bdevs/virtioblk_disk create virtioblk_user user $testdir/../../sample_block" "virtioblk_user" True "/var/tmp/virtio.sock"
$spdkcli_job "/bdevs/virtioscsi_disk create virtioscsi_user user $testdir/../../sample_scsi" "virtioscsi_user" True "/var/tmp/virtio.sock"
timing_exit spdkcli_create_virtio_user_config

timing_enter spdkcli_check_match
MATCH_FILE="spdkcli_virtio_user.test"
SPDKCLI_BRANCH="/vhost"
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_virtio_config
$spdkcli_job "/bdevs/virtioscsi_disk delete virtioscsi_user" "" False "/var/tmp/virtio.sock"
$spdkcli_job "/bdevs/virtioblk_disk delete virtioblk_user" "" False "/var/tmp/virtio.sock"
$spdkcli_job "/vhost/block delete sample_block" "sample_block"
$spdkcli_job "/vhost/scsi/sample_scsi remove_target 0" "Malloc0"
$spdkcli_job "/vhost/scsi delete sample_scsi" " sample_scsi"
if [ ! -z $pci_blk ]; then
	$spdkcli_job "/bdevs/virtioblk_disk delete virtioblk_pci" "virtioblk_pci"
fi
if [ ! -z $pci_scsi ]; then
	$spdkcli_job "/bdevs/virtioscsi_disk delete virtioscsi_pci" "virtioscsi_pci"
fi
$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
timing_exit spdkcli_clear_virtio_config

killprocess $virtio_pid
killprocess $spdk_tgt_pid

timing_exit spdk_cli_vhost
