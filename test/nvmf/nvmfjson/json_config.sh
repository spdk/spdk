#!/usr/bin/env bash
set -ex
NVMF_JSON_DIR=$(readlink -f $(dirname $0))
. $NVMF_JSON_DIR/../../json_config/common.sh
base_nvmf_config=$JSON_DIR/base_nvmf_config.json
last_nvmf_config=$JSON_DIR/last_nvmf_config.json

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt

	rootdir=$(readlink -f $NVMF_JSON_DIR/../..)
	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	create_nvmf_subsystem_config
	$rpc_py save_config -f $base_nvmf_config
	test_json_config

	clean_nvmf_subsystem_config
	test_params
	kill_targets

	run_spdk_tgt
	$rpc_py load_config -f $base_nvmf_config
	$rpc_py save_config -f $last_nvmf_config

	diff $base_nvmf_config $last_nvmf_config

	clean_nvmf_subsystem_config
	kill_targets
	clean_after_test_json_config
	rm -f $base_nvmf_config $last_nvmf_config
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
