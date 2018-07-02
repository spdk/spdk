#!/usr/bin/env bash
set -ex
VHOST_JSON_DIR=$(readlink -f $(dirname $0))
. $VHOST_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	run_spdk_tgt

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	$rpc_py start_subsystem_init

	create_pmem_bdev_subsytem_config
	test_json_config
	clear_pmem_bdev_subsystem_config

	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
timing_enter json_config_pmem

test_subsystems
timing_exit json_config_pmem
report_test_completion json_config_pmem
