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

timing_enter run_spcklithread
$spdkcli_job &
spdkcli_pid=$!
sleep 2
timing_exit run_spdkclithread

timing_enter spdkcli_create_bdevs_config
echo '"/bdevs/malloc create 40 512 Malloc0" "Malloc0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc create 32 512 Malloc1" "Malloc1" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc create 32 512 Malloc2" "Malloc2" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc create 32 4096 Malloc3" "Malloc3" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc create 32 4096 Malloc4" "Malloc4" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/error create Malloc1" "EE_Malloc1" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/error create Malloc4" "EE_Malloc4" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/null create null_bdev0 32 512" "null_bdev0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/null create null_bdev1 32 512" "null_bdev1" True' | nc -U /tmp/spdkcli.sock
dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
dd if=/dev/zero of=/tmp/sample_aio2 bs=2048 count=5000
echo '"/bdevs/aio create sample0 /tmp/sample_aio 512" "sample0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/aio create sample1 /tmp/sample_aio2 512" "sample1" True' | nc -U /tmp/spdkcli.sock
trtype=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").trtype')
traddr=$($SPDKCLI_BUILD_DIR/scripts/gen_nvme.sh --json | jq -r '.config[].params | select(.name=="Nvme0").traddr')
echo "\"/bdevs/nvme create Nvme0 $trtype $traddr\" \"Nvme0\" True" | nc -U /tmp/spdkcli.sock
echo '"/bdevs/split_disk split_bdev Nvme0n1 4" "Nvme0n1p0" True' | nc -U /tmp/spdkcli.sock
timing_exit spdkcli_create_bdevs_config

timing_enter spdkcli_create_lvols_config
echo '"/lvol_stores create lvs Malloc0" "lvs" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol0 16 lvs" "lvs/lvol0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol1 16 lvs" "lvs/lvol1" True' | nc -U /tmp/spdkcli.sock
timing_exit spdkcli_create_lvols_config

timing_enter spdkcli_create_vhosts_config
echo '"vhost/block create vhost_blk1 Nvme0n1p0" "Nvme0n1p0" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/block create vhost_blk2 Nvme0n1p1 0x1 readonly" "Nvme0n1p1" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi create vhost_scsi1" "vhost_scsi1" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi create vhost_scsi2" "vhost_scsi2" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi1 add_lun 0 Malloc2" "Malloc2" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi2 add_lun 0 Malloc3" "Malloc3" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi2 add_lun 1 Nvme0n1p2" "Nvme0n1p2" True' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi2 add_lun 2 Nvme0n1p3" "Nvme0n1p3" True' | nc -U /tmp/spdkcli.sock
timing_exit spdkcli_create_vhosts_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_save_config
echo "\"save_config $testdir/config.json"\" | nc -U /tmp/spdkcli.sock
echo "\"save_subsystem_config $testdir/config_bdev.json bdev"\" | nc -U /tmp/spdkcli.sock
echo "\"save_subsystem_config $testdir/config_vhost.json vhost"\" | nc -U /tmp/spdkcli.sock
timing_exit spdkcli_save_config

timing_enter spdkcli_check_match_details
$SPDKCLI_BUILD_DIR/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details | jq -r -S '.' > $testdir/match_files/spdkcli_details_vhost.test
$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/match_files/spdkcli_details_vhost.test.match
rm -f $testdir/match_files/spdkcli_details_vhost.test
timing_exit spdkcli_check_match_details

timing_enter spdkcli_clear_config
echo '"vhost/scsi/vhost_scsi2 remove_target 2" "Nvme0n1p3" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi2 remove_target 1" "Nvme0n1p2" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi2 remove_target 0" "Malloc3" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi/vhost_scsi1 remove_target 0" "Malloc2" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi delete vhost_scsi2" "vhost_scsi2" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/scsi delete vhost_scsi1" "vhost_scsi1" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/block delete vhost_blk2" "vhost_blk2" False' | nc -U /tmp/spdkcli.sock
echo '"vhost/block delete vhost_blk1" "vhost_blk1" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/split_disk destruct_split_bdev Nvme0n1" "Nvme0n1p0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/aio delete sample0" "sample0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/aio delete_all" "sample1" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/nvme delete Nvme0" "Nvme0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/null delete null_bdev0" "null_bdev0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/null delete_all" "null_bdev1" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume delete lvs/lvol0" "lvs/lvol0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume delete_all" "lvs/lvol1" False' | nc -U /tmp/spdkcli.sock
echo '"/lvol_stores delete lvs" "lvs" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/error delete EE_Malloc1" "EE_Malloc1" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/error delete_all" "EE_Malloc4" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc delete Malloc0" "Malloc0" False' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/malloc delete_all" "Malloc1" False' | nc -U /tmp/spdkcli.sock
timing_exit spdkcli_clear_config

timing_enter spdkcli_load_config
echo "\"load_config $testdir/config.json\"" | nc -U /tmp/spdkcli.sock
echo '"/lvol_stores create lvs Malloc0" "lvs" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol0 16 lvs" "lvs/lvol0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol1 16 lvs" "lvs/lvol1" True' | nc -U /tmp/spdkcli.sock
check_match
$spdk_clear_config_py clear_config
# FIXME: remove this sleep when NVMe driver will be fixed to wait for reset to complete
sleep 2
echo "\"load_subsystem_config $testdir/config_bdev.json\"" | nc -U /tmp/spdkcli.sock
echo "\"load_subsystem_config $testdir/config_vhost.json\"" | nc -U /tmp/spdkcli.sock
echo '"/lvol_stores create lvs Malloc0" "lvs" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol0 16 lvs" "lvs/lvol0" True' | nc -U /tmp/spdkcli.sock
echo '"/bdevs/logical_volume create lvol1 16 lvs" "lvs/lvol1" True' | nc -U /tmp/spdkcli.sock
check_match
$spdk_clear_config_py clear_config
rm -f $testdir/config.json
rm -f $testdir/config_bdev.json
rm -f $testdir/config_vhost.json
rm -f /tmp/sample_aio
timing_exit spdkcli_load_config

kill -9 $spdkcli_pid
killprocess $spdk_tgt_pid

timing_exit spdk_cli_vhost
report_test_completion spdk_cli_vhost
