#!/usr/bin/env bash
set -ex
VHOST_JSON_DIR=$(readlink -f $(dirname $0))
. $VHOST_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $VHOST_JSON_DIR/../../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	$rpc_py start_subsystem_init

	create_rbd_bdev_subsystem_config
	test_json_config
	clear_rbd_bdev_subsystem_config

	kill_targets
}

trap 'rbd_cleanup; on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
timing_enter rbd_json_config

test_subsystems
timing_exit rbd_json_config
report_test_completion rbd_json_config
