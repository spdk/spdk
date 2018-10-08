#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli_iscsi.test"
SPDKCLI_BRANCH="/iscsi"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh
. $testdir/../iscsi_tgt/common.sh

timing_enter spdkcli_iscsi
trap 'on_error_exit;' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_iscsi_config
$spdkcli_job "/bdevs/malloc create 32 512 Malloc0" "Malloc0" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc3" "Malloc3" True
$spdkcli_job "/iscsi/portal_groups create 1 \"127.0.0.1:3261 127.0.0.1:3263@0x1\"" "host=127.0.0.1, port=3261" True
$spdkcli_job "/iscsi/portal_groups create 2 127.0.0.1:3262" "host=127.0.0.1, port=3262" True
$spdkcli_job "/iscsi/initiator_groups create 2 ANY 10.0.2.15/32" "hostname=ANY, netmask=10.0.2.15/32" True
$spdkcli_job "/iscsi/initiator_groups create 3 ANZ 10.0.2.15/32" "hostname=ANZ, netmask=10.0.2.15/32" True
$spdkcli_job "/iscsi/initiator_groups add_initiator 2 ANW 10.0.2.16/32" "hostname=ANW, netmask=10.0.2.16" True
$spdkcli_job "/iscsi/target_nodes create Target0 Target0_alias \"Malloc0:0 Malloc1:1\" 1:2 64 g=1" "Target0" True
$spdkcli_job "/iscsi/target_nodes create Target1 Target1_alias Malloc2:0 1:2 64 g=1" "Target1" True
$spdkcli_job "/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 add_pg_ig_maps \"1:3 2:2\"" "portal_group1 - initiator_group3" True
$spdkcli_job "/iscsi/target_nodes add_lun iqn.2016-06.io.spdk:Target1 Malloc3 2" "Malloc3" True
$spdkcli_job "/iscsi/auth_groups create 1 \"user:test secret:test muser:mutual_test msecret:mutual_test \
user:test3 secret:test3 muser:mutual_test3 msecret:mutual_test3\"" "user=test3" True
$spdkcli_job "/iscsi/auth_groups add_secret 1 user=test2 secret=test2 muser=mutual_test2 msecret=mutual_test2" "user=test2" True
$spdkcli_job "/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 set_auth g=1 d=true" "disable_chap: True" True
$spdkcli_job "/iscsi/global_params set_auth g=1 d=true r=false" "disable_chap: True" True
$spdkcli_job "/iscsi ls" "Malloc" True
timing_exit spdkcli_create_iscsi_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_iscsi_config
$spdkcli_job "/iscsi/auth_groups delete_secret 1 test2" "user=test2"
$spdkcli_job "/iscsi/auth_groups delete 1" "user=test"
$spdkcli_job "/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 delete_pg_ig_maps \"1:3 2:2\"" "portal_group1 - initiator_group3"
$spdkcli_job "/iscsi/target_nodes delete iqn.2016-06.io.spdk:Target1" "Target1"
$spdkcli_job "/iscsi/target_nodes delete iqn.2016-06.io.spdk:Target0" "Target0"
$spdkcli_job "/iscsi/initiator_groups delete_initiator 2 ANW 10.0.2.16/32" "ANW"
$spdkcli_job "/iscsi/initiator_groups delete 3" "ANYZ"
$spdkcli_job "/iscsi/portal_groups delete 1" "127.0.0.1:3261"
$spdkcli_job "/bdevs/malloc delete Malloc3" "Malloc3"
$spdkcli_job "/bdevs/malloc delete Malloc2" "Malloc2"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
$spdkcli_job "/bdevs/malloc delete Malloc0" "Malloc0"
timing_exit spdkcli_clear_iscsi_config

killprocess $spdk_tgt_pid
timing_exit spdkcli_iscsi
report_test_completion spdk_cli
