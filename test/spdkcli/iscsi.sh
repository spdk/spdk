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
$spdkcli_job "/iscsi/scsi_devices create Target0 Target0_alias Malloc0:0#Malloc1:1 1:2 64 d=1" "Malloc0" True
$spdkcli_job "/iscsi/scsi_devices create Target1 Target1_alias Malloc2:0 1:2 64 d=1" "Malloc2" True
$spdkcli_job "/iscsi ls" "" True
timing_exit spdkcli_create_iscsi_config

timing_enter spdkcli_check_match
#check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_iscsi_config
$spdkcli_job "/bdevs/pmemblk delete pmem_bdev" "pmem_bdev"
$spdkcli_job "iscs delete_pmem_pool /tmp/sample_pmem" ""
timing_exit spdkcli_clear_iscsi_config

killprocess $spdk_tgt_pid
timing_exit spdkcli_iscsi
report_test_completion spdk_cli

