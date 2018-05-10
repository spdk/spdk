#!/usr/bin/env bash
set -ex
VHOST_JSON_DIR=$(readlink -f $(dirname $0))
. $VHOST_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $VHOST_JSON_DIR/../../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme

	upload_vhost
	test_json_config
	clean_vhost

	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
