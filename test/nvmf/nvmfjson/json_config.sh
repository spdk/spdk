#!/usr/bin/env bash
set -xe
NVMF_JSON_DIR=$(readlink -f $(dirname $0))
. $NVMF_JSON_DIR/../../json_config/common.sh
base_nvmf_config=$JSON_DIR/base_nvmf_config.json
last_nvmf_config=$JSON_DIR/last_nvmf_config.json

function test_subsystems() {
	run_spdk_tgt

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"

	$rpc_py start_subsystem_init
	create_nvmf_subsystem_config
	$rpc_py save_config > $base_nvmf_config
	test_json_config

	clear_nvmf_subsystem_config
	kill_targets

	run_spdk_tgt
	$rpc_py load_config < $base_nvmf_config
	$rpc_py save_config > $last_nvmf_config

	json_diff $base_nvmf_config $last_nvmf_config

	clear_nvmf_subsystem_config
	kill_targets
	rm -f $base_nvmf_config $last_nvmf_config
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"; rm -f $base_nvmf_config $last_nvmf_config' ERR

timing_enter nvmf_json_config
test_subsystems
timing_exit nvmf_json_config
revert_soft_roce

report_test_completion nvmf_json_config
