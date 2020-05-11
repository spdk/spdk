#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/spdkcli/common.sh
source $rootdir/test/iscsi_tgt/common.sh

MATCH_FILE="spdkcli_iscsi.test"
SPDKCLI_BRANCH="/iscsi"

trap 'on_error_exit;' ERR

timing_enter run_iscsi_tgt

# Running iscsi target with --wait-for-rpc. Implies framework_start_init later
$SPDK_BIN_DIR/iscsi_tgt -m 0x3 -p 0 -s 4096 --wait-for-rpc &
iscsi_tgt_pid=$!
waitforlisten $iscsi_tgt_pid
$rootdir/scripts/rpc.py framework_start_init

timing_exit run_iscsi_tgt

timing_enter spdkcli_create_iscsi_config
$spdkcli_job "'/bdevs/malloc create 32 512 Malloc0' 'Malloc0' True
'/bdevs/malloc create 32 512 Malloc1' 'Malloc1' True
'/bdevs/malloc create 32 512 Malloc2' 'Malloc2' True
'/bdevs/malloc create 32 512 Malloc3' 'Malloc3' True
'/iscsi/portal_groups create 1 \"127.0.0.1:3261 127.0.0.1:3263@0x1\"' 'host=127.0.0.1, port=3261' True
'/iscsi/portal_groups create 2 127.0.0.1:3262' 'host=127.0.0.1, port=3262' True
'/iscsi/initiator_groups create 2 ANY 10.0.2.15/32' 'hostname=ANY, netmask=10.0.2.15/32' True
'/iscsi/initiator_groups create 3 ANZ 10.0.2.15/32' 'hostname=ANZ, netmask=10.0.2.15/32' True
'/iscsi/initiator_groups add_initiator 2 ANW 10.0.2.16/32' 'hostname=ANW, netmask=10.0.2.16' True
'/iscsi/target_nodes create Target0 Target0_alias \"Malloc0:0 Malloc1:1\" 1:2 64 g=1' 'Target0' True
'/iscsi/target_nodes create Target1 Target1_alias Malloc2:0 1:2 64 g=1' 'Target1' True
'/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 iscsi_target_node_add_pg_ig_maps \"1:3 2:2\"' 'portal_group1 - initiator_group3' True
'/iscsi/target_nodes add_lun iqn.2016-06.io.spdk:Target1 Malloc3 2' 'Malloc3' True
'/iscsi/auth_groups create 1 \"user:test1 secret:test1 muser:mutual_test1 msecret:mutual_test1,\
user:test3 secret:test3 muser:mutual_test3 msecret:mutual_test3\"' 'user=test3' True
'/iscsi/auth_groups add_secret 1 user=test2 secret=test2 muser=mutual_test2 msecret=mutual_test2' 'user=test2' True
'/iscsi/auth_groups create 2 \"user:test4 secret:test4 muser:mutual_test4 msecret:mutual_test4\"' 'user=test4' True
'/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 set_auth g=1 d=true' 'disable_chap: True' True
'/iscsi/global_params set_auth g=1 d=true r=false' 'disable_chap: True' True
'/iscsi ls' 'Malloc' True
"
timing_exit spdkcli_create_iscsi_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_iscsi_config
$spdkcli_job "'/iscsi/auth_groups delete_secret 1 test2' 'user=test2'
'/iscsi/auth_groups delete_secret_all 1' 'user=test1'
'/iscsi/auth_groups delete 1' 'user=test1'
'/iscsi/auth_groups delete_all' 'user=test4'
'/iscsi/target_nodes/iqn.2016-06.io.spdk:Target0 iscsi_target_node_remove_pg_ig_maps \"1:3 2:2\"' 'portal_group1 - initiator_group3'
'/iscsi/target_nodes delete iqn.2016-06.io.spdk:Target1' 'Target1'
'/iscsi/target_nodes delete_all' 'Target0'
'/iscsi/initiator_groups delete_initiator 2 ANW 10.0.2.16/32' 'ANW'
'/iscsi/initiator_groups delete 3' 'ANZ'
'/iscsi/initiator_groups delete_all' 'ANY'
'/iscsi/portal_groups delete 1' '127.0.0.1:3261'
'/iscsi/portal_groups delete_all' '127.0.0.1:3262'
'/bdevs/malloc delete Malloc3' 'Malloc3'
'/bdevs/malloc delete Malloc2' 'Malloc2'
'/bdevs/malloc delete Malloc1' 'Malloc1'
'/bdevs/malloc delete Malloc0' 'Malloc0'
"
timing_exit spdkcli_clear_iscsi_config

killprocess $iscsi_tgt_pid
