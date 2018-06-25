#!/usr/bin/env bash
set -ex
ISCSI_JSON_DIR=$(readlink -f $(dirname $0))
. $ISCSI_JSON_DIR/../../json_config/common.sh
base_iscsi_config=$JSON_DIR/base_iscsi_config.json
last_iscsi_config=$JSON_DIR/last_iscsi_config.json

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt

	rootdir=$(readlink -f $ISCSI_JSON_DIR/../..)
	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	create_iscsi_subsystem_config
	$rpc_py save_config -f $base_iscsi_config
	test_json_config

	clean_iscsi_subsystem_config
	test_params
	kill_targets

	run_spdk_tgt
	$rpc_py load_config -f $base_iscsi_config
	$rpc_py save_config -f $last_iscsi_config

	diff $base_iscsi_config $last_iscsi_config

	clean_iscsi_subsystem_config
	kill_targets
	clean_after_test_json_config
	rm -f $base_iscsi_config $last_iscsi_config
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

timing_enter iscsi_json_config
test_subsystems
timing_exit iscsi_json_config

rmmod nbd
report_test_completion iscsi_json_config
