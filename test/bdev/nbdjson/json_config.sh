#!/usr/bin/env bash
set -xe
NBD_JSON_DIR=$(readlink -f $(dirname $0))
. $NBD_JSON_DIR/../../json_config/common.sh
rpc_py="$spdk_rpc_py"
clear_config_py="$spdk_clear_config_py"
trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

timing_enter nbd_json_config
run_spdk_tgt
load_nvme
modprobe nbd

timing_enter nbd_json_config_create_setup
$rpc_py construct_malloc_bdev 128 512 --name Malloc0
$rpc_py start_nbd_disk Malloc0 /dev/nbd0
$rpc_py start_nbd_disk Nvme0n1 /dev/nbd1
timing_exit nbd_json_config_create_setup

timing_enter nbd_json_config_test
test_json_config
timing_exit nbd_json_config_test

$clear_config_py clear_config
kill_targets
rmmod nbd
timing_exit nbd_json_config
report_test_completion nbd_json_config
