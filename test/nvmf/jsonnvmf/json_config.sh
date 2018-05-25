#!/usr/bin/env bash
set -ex
NVMF_JSON_DIR=$(readlink -f $(dirname $0))
. $NVMF_JSON_DIR/../../json_config/common.sh

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh "--json" | $SPDK_BUILD_DIR/scripts/rpc.py\
		 -s /var/tmp/spdk.sock load_subsystem_config
	rootdir=$(readlink -f $NVMF_JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	create_nvmf_subsystem_config
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $base_json_config
	kill_targets

	run_spdk_tgt -w
	$rpc_py load_config -f $base_json_config

	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $last_bdevs
	$rpc_py save_config -f $last_json_config

	if [ "$(diff $base_bdevs $last_bdevs)" != "" ]; then
		echo "Bdevs are different"
		return 1
	fi
	if [ "$(diff $base_json_config $last_json_config)" != "" ]; then
		echo "Two configs are different"
		return 1
	fi

	kill_targets
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
