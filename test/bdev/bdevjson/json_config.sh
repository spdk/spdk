#!/usr/bin/env bash
set -ex
BDEV_JSON_DIR=$(readlink -f $(dirname $0))
. $BDEV_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	rootdir=$(readlink -f $BDEV_JSON_DIR/../../..)
	objdump -d $rootdir/app/spdk_tgt/spdk_tgt | grep "^0000"
	run_spdk_tgt

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme
	create_bdev_subsystem_config
	test_json_config

	clear_bdev_subsystem_config
	test_global_params "spdk_tgt"
	kill_targets
}

timing_enter json_config
trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

test_subsystems

timing_exit json_config
report_test_completion json_config
