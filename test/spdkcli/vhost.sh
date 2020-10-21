#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/spdkcli/common.sh

MATCH_FILE="spdkcli_vhost.test"
SPDKCLI_BRANCH="/"

sample_aio=$SPDK_TEST_STORAGE/sample_aio
sample_aio2=$SPDK_TEST_STORAGE/sample_aio2

trap 'on_error_exit' ERR
timing_enter run_vhost_tgt
run_vhost_tgt
timing_exit run_vhost_tgt

timing_enter spdkcli_create_bdevs_config
$spdkcli_job "'/bdevs/malloc create 40 512 Malloc0' 'Malloc0' True
'/bdevs/malloc create 32 512 Malloc1' 'Malloc1' True
'/bdevs/malloc create 32 512 Malloc2' 'Malloc2' True
'/bdevs/malloc create 32 4096 Malloc3' 'Malloc3' True
'/bdevs/malloc create 32 4096 Malloc4' 'Malloc4' True
'/bdevs/malloc create 32 4096 Malloc5' 'Malloc5' True
'/bdevs/error create Malloc1' 'EE_Malloc1' True
'/bdevs/error create Malloc4' 'EE_Malloc4' True
'/bdevs/null create null_bdev0 32 512' 'null_bdev0' True
'/bdevs/null create null_bdev1 32 512' 'null_bdev1' True
"
dd if=/dev/zero of="$sample_aio" bs=2048 count=5000
dd if=/dev/zero of="$sample_aio2" bs=2048 count=5000
$spdkcli_job "'/bdevs/aio create sample0 $sample_aio 512' 'sample0' True
'/bdevs/aio create sample1 $sample_aio2 512' 'sample1' True
"
trtype=$($rootdir/scripts/gen_nvme.sh | jq -r '.config[].params | select(.name=="Nvme0").trtype')
traddr=$($rootdir/scripts/gen_nvme.sh | jq -r '.config[].params | select(.name=="Nvme0").traddr')
$spdkcli_job "'/bdevs/nvme create Nvme0 $trtype $traddr' 'Nvme0' True
'/bdevs/split_disk bdev_split_create Nvme0n1 4' 'Nvme0n1p0' True
"
timing_exit spdkcli_create_bdevs_config

timing_enter spdkcli_create_lvols_config
$spdkcli_job "'/lvol_stores create lvs0 Malloc0' 'lvs0' True
'/lvol_stores create lvs1 Malloc5' 'lvs1' True
'/bdevs/logical_volume create lvol0 16 lvs0' 'lvs0/lvol0' True
'/bdevs/logical_volume create lvol1 16 lvs0' 'lvs0/lvol1' True
"
timing_exit spdkcli_create_lvols_config

timing_enter spdkcli_check_match_details
$rootdir/scripts/spdkcli.py /lvol_stores/lvs0 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_lvs.test
$rootdir/test/app/match/match $testdir/match_files/spdkcli_details_lvs.test.match
rm -f $testdir/match_files/spdkcli_details_lvs.test
timing_exit spdkcli_check_match_details

timing_enter spdkcli_create_vhosts_config
$spdkcli_job "'vhost/block create vhost_blk1 Nvme0n1p0' 'Nvme0n1p0' True
'vhost/block create vhost_blk2 Nvme0n1p1 0x1 readonly' 'Nvme0n1p1' True
'vhost/scsi create vhost_scsi1' 'vhost_scsi1' True
'vhost/scsi create vhost_scsi2' 'vhost_scsi2' True
'vhost/scsi/vhost_scsi1 add_lun 0 Malloc2' 'Malloc2' True
'vhost/scsi/vhost_scsi2 add_lun 0 Malloc3' 'Malloc3' True
'vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2' 'Nvme0n1p2' True
'vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3' 'Nvme0n1p3' True
'vhost/scsi/vhost_scsi1 set_coalescing 20 1000000' '' True
"
timing_exit spdkcli_create_vhosts_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_save_config
$spdkcli_job "'save_config $testdir/config.json'
'save_subsystem_config $testdir/config_bdev.json bdev'
'save_subsystem_config $testdir/config_vhost.json vhost'
"
timing_exit spdkcli_save_config

timing_enter spdkcli_check_match_details
$rootdir/scripts/spdkcli.py vhost/scsi/vhost_scsi1/Target_0 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_vhost_target.test
$rootdir/test/app/match/match $testdir/match_files/spdkcli_details_vhost_target.test.match
rm -f $testdir/match_files/spdkcli_details_vhost_target.test

$rootdir/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_vhost.test
$rootdir/test/app/match/match $testdir/match_files/spdkcli_details_vhost.test.match
rm -f $testdir/match_files/spdkcli_details_vhost.test

$rootdir/scripts/spdkcli.py vhost/scsi/vhost_scsi1 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_vhost_ctrl.test
$rootdir/test/app/match/match $testdir/match_files/spdkcli_details_vhost_ctrl.test.match
rm -f $testdir/match_files/spdkcli_details_vhost_ctrl.test
timing_exit spdkcli_check_match_details

timing_enter spdkcli_clear_config
$spdkcli_job "'vhost/scsi/vhost_scsi2 remove_target 2' 'Nvme0n1p3'
'vhost/scsi/vhost_scsi2 remove_target 1' 'Nvme0n1p2'
'vhost/scsi/vhost_scsi2 remove_target 0' 'Malloc3'
'vhost/scsi/vhost_scsi1 remove_target 0' 'Malloc2'
'vhost/scsi delete vhost_scsi2' 'vhost_scsi2'
'vhost/scsi delete vhost_scsi1' 'vhost_scsi1'
'vhost/block delete vhost_blk2' 'vhost_blk2'
'vhost/block delete vhost_blk1' 'vhost_blk1'
'/bdevs/split_disk bdev_split_delete Nvme0n1' 'Nvme0n1p0'
'/bdevs/aio delete sample0' 'sample0'
'/bdevs/aio delete_all' 'sample1'
'/bdevs/nvme delete Nvme0' 'Nvme0'
'/bdevs/null delete null_bdev0' 'null_bdev0'
'/bdevs/null delete_all' 'null_bdev1'
'/bdevs/logical_volume delete lvs0/lvol0' 'lvs0/lvol0'
'/bdevs/logical_volume delete_all' 'lvs0/lvol1'
'/lvol_stores delete lvs0' 'lvs0'
'/lvol_stores delete_all' 'lvs1'
'/bdevs/error delete EE_Malloc1' 'EE_Malloc1'
'/bdevs/error delete_all' 'EE_Malloc4'
'/bdevs/malloc delete Malloc0' 'Malloc0'
'/bdevs/malloc delete_all' 'Malloc1'
"
timing_exit spdkcli_clear_config

timing_enter spdkcli_load_config
$spdkcli_job "'load_config $testdir/config.json'
'/lvol_stores create lvs0 Malloc0' 'lvs0' True
'/lvol_stores create lvs1 Malloc5' 'lvs1' True
'/bdevs/logical_volume create lvol0 16 lvs0' 'lvs0/lvol0' True
'/bdevs/logical_volume create lvol1 16 lvs0' 'lvs0/lvol1' True
"
check_match
$spdk_clear_config_py clear_config
# FIXME: remove this sleep when NVMe driver will be fixed to wait for reset to complete
sleep 2
$spdkcli_job "'load_subsystem_config $testdir/config_bdev.json'
'load_subsystem_config $testdir/config_vhost.json'
'/lvol_stores create lvs0 Malloc0' 'lvs0' True
'/lvol_stores create lvs1 Malloc5' 'lvs1' True
'/bdevs/logical_volume create lvol0 16 lvs0' 'lvs0/lvol0' True
'/bdevs/logical_volume create lvol1 16 lvs0' 'lvs0/lvol1' True
"
check_match
$spdk_clear_config_py clear_config
rm -f $testdir/config.json
rm -f $testdir/config_bdev.json
rm -f $testdir/config_vhost.json
rm -f "$sample_aio" "$sample_aio2"
timing_exit spdkcli_load_config

killprocess $vhost_tgt_pid
