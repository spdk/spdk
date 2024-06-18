#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation
#  All rights reserved.
#

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"
source "$rootdir/test/json_config/common.sh"

declare -A app_pid=([target]="")
declare -A app_socket=([target]='/var/tmp/spdk_tgt.sock')
declare -A configs_path=([target]="$rootdir/test/json_config/tgt_config.json")

cleanup() {
	if [[ -n "${app_pid[target]}" ]]; then
		killprocess ${app_pid[target]}
	fi
	rm -f "${configs_path[@]}"
}

trap 'cleanup' ERR EXIT

echo "INFO: launching target and waiting for RPC..."
json_config_test_start_app target --wait-for-rpc

waitforlisten ${app_pid[target]} ${app_socket[target]}
tgt_rpc rpc_get_methods -c | grep -q "framework_start_init"

# Ensure we are not in SPDK_RPC_RUNTIME yet.
NOT tgt_rpc bdev_get_bdevs &> /dev/null

# Set additional configuration.
tgt_rpc bdev_set_options -d -p 8192 -c 128

tgt_rpc framework_start_init

echo "Waiting for target to run..."
waitforlisten ${app_pid[target]} ${app_socket[target]}
tgt_rpc framework_wait_init

# We must be in SPDK_RPC_RUNTIME now.
tgt_rpc bdev_get_bdevs &> /dev/null

tgt_rpc save_config > ${configs_path[target]}

json_config_test_shutdown_app target

json_config_test_start_app target --wait-for-rpc --json ${configs_path[target]}
waitforlisten ${app_pid[target]} ${app_socket[target]}
tgt_rpc rpc_get_methods -c | grep -q "framework_start_init"

# Override configuration provided with JSON file.
expected_auto_examine="true"
expected_io_pool_size="16384"
expected_io_cache_size="256"
tgt_rpc bdev_set_options -e -p $expected_io_pool_size -c $expected_io_cache_size
tgt_rpc framework_start_init

# Check if the configuration override was successful.
jq_args='.[] | select(.method=="bdev_set_options").params'
bdev_set_options_params=$(tgt_rpc framework_get_config bdev | jq -r "$jq_args")
set_auto_examine=$(echo $bdev_set_options_params | jq .bdev_auto_examine)
set_io_pool_size=$(echo $bdev_set_options_params | jq .bdev_io_pool_size)
set_io_cache_size=$(echo $bdev_set_options_params | jq .bdev_io_cache_size)

[ "$set_auto_examine" = "$expected_auto_examine" ]
[ "$set_io_pool_size" = "$expected_io_pool_size" ]
[ "$set_io_cache_size" = "$expected_io_cache_size" ]

echo "INFO: shutting down applications..."
json_config_test_shutdown_app target

echo "Success"
