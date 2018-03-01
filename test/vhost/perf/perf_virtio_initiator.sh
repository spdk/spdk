#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

source $COMMON_DIR/common.sh

PLUGIN_DIR_BDEV=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
virtio_bdevs=""

function run_spdk_virtio_fio() {
	LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/perf.fio --ioengine=spdk_bdev\
	 --spdk_conf=$BASE_DIR/bdev.conf "$@" --spdk_mem=1024\
	 --output-format=json --cpumask=3
}

timing_enter spdk_vhost_run
spdk_vhost_run --conf-path=$BASE_DIR
timing_exit spdk_vhost_run

$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1.0
$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

run_spdk_virtio_fio  --filename=$virtio_bdevs  "$@"

timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
