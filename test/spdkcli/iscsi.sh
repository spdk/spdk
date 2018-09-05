#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli_iscsi.test"
SPDKCLI_BRANCH="/bdevs/pmemblk"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh
. $testdir/../iscsi_tgt/common.sh

timing_enter spdkcli_pmem
trap 'on_error_exit;' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_iscsi_config
$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc3" "Malloc3" True
$spdkcli_job "/iscsi/portal_groups create 1 127.0.0.1:3261" "127.0.0.1:3261" True
$spdkcli_job "/iscsi/initiator_groups create 2 ANY 10.0.2.15/32" "ANY" True
$spdkcli_job "/iscsi/scsi_devices create Target0 Target0_alias Malloc0:0-Malloc1:1 1:2 64 d=1" "Target0" True
$spdkcli_job "/iscsi/scsi_devices create Target1 Target1_alias Malloc2:0 1:2 64 d=1" "Target1" True
$spdkcli_job "/iscsi/scsi_devices/iqn.2016-06.io.spdk:Target0 add_lun iqn.2016-06.io.spdk:Target1 Malloc3 2" "Malloc3"
timing_exit spdkcli_create_iscsi_config

timing_enter spdkcli_check_match
#check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_iscsi_config
$spdkcli_job "/iscsi/scsi_devices delete iqn.2016-06.io.spdk:Target1" "Target1"
$spdkcli_job "/iscsi/scsi_devices delete iqn.2016-06.io.spdk:Target0" "Target0"
$spdkcli_job "/iscsi/initiator_groups delete 2" "ANY"
$spdkcli_job "/iscsi/portal_groups delete 1" "127.0.0.1:3261"
$spdkcli_job "/bdevs/malloc delete Malloc3" "Malloc3"
$spdkcli_job "/bdevs/malloc delete Malloc2" "Malloc2"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
timing_exit spdkcli_clear_iscsi_config

killprocess $spdk_tgt_pid
timing_exit spdkcli_iscsi
report_test_completion spdk_cli

