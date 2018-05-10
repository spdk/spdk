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
	$clear_config_py clear_config

	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
timing_enter json_config_vhost

test_subsystems
timing_exit json_config_vhost
report_test_completion json_config_vhost
