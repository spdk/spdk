#!/usr/bin/env bash

set -e
SHARED_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $SHARED_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $SHARED_DIR/../../..)
source $COMMON_DIR/common.sh
PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
FIO_PATH="/usr/src/fio"
rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

function run_spdk_fio() {
	LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_PATH/fio $1 --ioengine=spdk_bdev\
	--spdk_conf=$SHARED_DIR/bdev.conf --spdk_mem=1024 --spdk_single_seg=1
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR SIGTERM SIGABRT

spdk_vhost_run

$rpc_py construct_malloc_bdev -b Malloc 124 4096
$rpc_py construct_vhost_blk_controller Malloc.0 Malloc

run_spdk_fio "$SHARED_DIR/default_shared.job --section=write_0 --filename=VirtioBlk0" &
run_fio_pid=$!
sleep 2
run_spdk_fio "$SHARED_DIR/default_shared.job --section=write_1 --section=verify_0 --filename=VirtioBlk0"
wait $run_fio_pid
spdk_vhost_kill
