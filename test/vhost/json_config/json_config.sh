#!/usr/bin/env bash
set -ex
VHOST_JSON_DIR=$(readlink -f $(dirname $0))
. $VHOST_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	run_spdk_tgt

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	upload_vhost
	test_json_config
	$rpc_py save_config > $full_config
	$clear_config_py clear_config

	kill_targets

	run_spdk_tgt --json $full_config
	$rpc_py save_config | $JSON_DIR/config_filter.py -method "delete_global_parameters" > $last_json_config
	json_diff $base_json_config $last_json_config
	remove_config_files_after_test_json_config
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
timing_enter json_config_vhost

test_subsystems
timing_exit json_config_vhost
report_test_completion json_config_vhost
