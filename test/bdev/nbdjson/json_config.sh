#!/usr/bin/env bash
set -xe
NBD_JSON_DIR=$(readlink -f $(dirname $0))
. $NBD_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	run_spdk_tgt

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	modprobe nbd
	create_nbd_subsystem_config
	test_json_config

	clear_nbd_subsystem_config
	kill_targets
	rmmod nbd
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

timing_enter nbd_json_config
test_subsystems
timing_exit nbd_json_config

report_test_completion nbd_json_config
