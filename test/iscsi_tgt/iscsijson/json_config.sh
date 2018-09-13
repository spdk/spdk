#!/usr/bin/env bash
set -xe
ISCSI_JSON_DIR=$(readlink -f $(dirname $0))
. $ISCSI_JSON_DIR/../../json_config/common.sh
. $JSON_DIR/../iscsi_tgt/common.sh
base_iscsi_config=$JSON_DIR/base_iscsi_config.json
last_iscsi_config=$JSON_DIR/last_iscsi_config.json
rpc_py="$spdk_rpc_py"
clear_config_py="$spdk_clear_config_py"
trap 'on_error_exit "${FUNCNAME}" "${LINENO}"; rm -f $base_iscsi_config $last_iscsi_config' ERR

timing_enter iscsi_json_config
run_spdk_tgt
$rpc_py start_subsystem_init

timing_enter iscsi_json_config_create_setup
$rpc_py add_portal_group $PORTAL_TAG 127.0.0.1:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev 64 4096 --name Malloc0
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
$rpc_py save_config > $base_iscsi_config
timing_exit iscsi_json_config_create_setup

timing_enter iscsi_json_config_test
test_json_config
timing_exit iscsi_json_config_test

timing_enter iscsi_json_config_restart_spdk
$clear_config_py clear_config
kill_targets
run_spdk_tgt
$rpc_py load_config < $base_iscsi_config
$rpc_py save_config > $last_iscsi_config
timing_exit iscsi_json_config_restart_spdk

json_diff $base_iscsi_config $last_iscsi_config

$clear_config_py clear_config
kill_targets
rm -f $base_iscsi_config $last_iscsi_config

timing_exit iscsi_json_config
report_test_completion iscsi_json_config
