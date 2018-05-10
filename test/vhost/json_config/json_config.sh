#!/usr/bin/env bash
set -ex
VHOST_JSON_DIR=$(readlink -f $(dirname $0))
. $VHOST_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	echo "WARNING: Export flag to skip bug in librados"
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt
	rootdir=$(readlink -f $VHOST_JSON_DIR/../../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	load_nvme
	create_bdev_subsystem_config

	upload_vhost
	test_json_config
	clean_vhost

	clean_bdev_subsystem_config
	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
