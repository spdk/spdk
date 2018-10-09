#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli_vhost.test"
SPDKCLI_BRANCH="/"
testdir=$(readlink -f $(dirname $0))
. $testdir/../json_config/common.sh
. $testdir/common.sh

timing_enter spdk_cli_vhost
trap 'on_error_exit' ERR
timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_bdevs_config
$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
$spdkcli_job "/bdevs/malloc create 32 4096 Malloc3" "Malloc3" True
$spdkcli_job "/bdevs/error create Malloc1" "EE_Malloc1" True
$spdkcli_job "/bdevs/null create null_bdev 32 512" "null_bdev" True
dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
$spdkcli_job "/bdevs/aio create sample /tmp/sample_aio 512" "sample" True
trtype=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").trtype')
traddr=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
$spdkcli_job "/bdevs/nvme create Nvme0 $trtype $traddr" "Nvme0" True
$spdkcli_job "/bdevs/split_disk split_bdev Nvme0n1 4" "Nvme0n1p0" True
timing_exit spdkcli_create_bdevs_config

timing_enter spdkcli_create_lvols_config
$spdkcli_job "/lvol_stores create lvs Malloc0" "lvs" True
$spdkcli_job "/bdevs/logical_volume create lvol 16 lvs" "lvs/lvol" True
timing_exit spdkcli_create_lvols_config

timing_enter spdkcli_create_vhosts_config
$spdkcli_job "vhost/block create vhost_blk1 Nvme0n1p0" "Nvme0n1p0" True
$spdkcli_job "vhost/block create vhost_blk2 Nvme0n1p1 0x2 readonly" "Nvme0n1p1" True
$spdkcli_job "vhost/scsi create vhost_scsi1" "vhost_scsi1" True
$spdkcli_job "vhost/scsi create vhost_scsi2" "vhost_scsi2" True
$spdkcli_job "vhost/scsi/vhost_scsi1 add_lun 0 Malloc2" "Malloc2" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 0 Malloc3" "Malloc3" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2" "Nvme0n1p2" True
$spdkcli_job "vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3" "Nvme0n1p3" True
timing_exit spdkcli_create_vhosts_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_save_config
$spdkcli_job "save_config $testdir/config.json"
$spdkcli_job "save_subsystem_config $testdir/config_bdev.json bdev"
$spdkcli_job "save_subsystem_config $testdir/config_vhost.json vhost"
timing_exit spdkcli_save_config

timing_enter spdkcli_check_match_details
$SPDKCLI_BUILD_DIR/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_vhost.test
$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/match_files/spdkcli_details_vhost.test.match
rm -f $testdir/match_files/spdkcli_details_vhost.test
timing_exit spdkcli_check_match_details

timing_enter spdkcli_clear_config
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 2" "Nvme0n1p3"
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 1" "Nvme0n1p2"
$spdkcli_job "vhost/scsi/vhost_scsi2 remove_target 0" "Malloc3"
$spdkcli_job "vhost/scsi/vhost_scsi1 remove_target 0" "Malloc2"
$spdkcli_job "vhost/scsi delete vhost_scsi2" "vhost_scsi2"
$spdkcli_job "vhost/scsi delete vhost_scsi1" "vhost_scsi1"
$spdkcli_job "vhost/block delete vhost_blk2" "vhost_blk2"
$spdkcli_job "vhost/block delete vhost_blk1" "vhost_blk1"
$spdkcli_job "/bdevs/split_disk destruct_split_bdev Nvme0n1" "Nvme0n1p0"
$spdkcli_job "/bdevs/aio delete sample" "sample"
$spdkcli_job "/bdevs/nvme delete Nvme0" "Nvme0"
$spdkcli_job "/bdevs/null delete null_bdev" "null_bdev"
$spdkcli_job "/bdevs/logical_volume delete lvs/lvol" "lvs/lvol"
$spdkcli_job "/lvol_stores delete lvs" "lvs"
$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
$spdkcli_job "/bdevs/malloc delete Malloc2" "Malloc2"
$spdkcli_job "/bdevs/malloc delete Malloc3" "Malloc3"
timing_exit spdkcli_clear_config

timing_enter spdkcli_load_config
$spdkcli_job "load_config $testdir/config.json"
$spdkcli_job "/lvol_stores create lvs Malloc0" "lvs" True
$spdkcli_job "/bdevs/logical_volume create lvol 16 lvs" "lvs/lvol" True
check_match
$spdk_clear_config_py clear_config
# FIXME: remove this sleep when NVMe driver will be fixed to wait for reset to complete
sleep 2
$spdkcli_job "load_subsystem_config $testdir/config_bdev.json"
$spdkcli_job "load_subsystem_config $testdir/config_vhost.json"
$spdkcli_job "/lvol_stores create lvs Malloc0" "lvs" True
$spdkcli_job "/bdevs/logical_volume create lvol 16 lvs" "lvs/lvol" True
check_match
rm -f $testdir/config.json
rm -f $testdir/config_bdev.json
rm -f $testdir/config_vhost.json
rm -f /tmp/sample_aio
timing_exit spdkcli_load_config

killprocess $spdk_tgt_pid

timing_exit spdk_cli_vhost
report_test_completion spdk_cli_vhost
