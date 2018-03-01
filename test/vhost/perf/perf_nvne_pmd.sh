#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR_NVME=$ROOT_DIR/examples/nvme/fio_plugin

function run_spdk_nvme_fio(){
	$FIO_BIN $BASE_DIR/perf.fio --output-format=json\
	 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin --cpumask=1
}

name=$(lspci | grep -i Non | awk '{print $1}')
name=${name//[:]/.}
filename='trtype=PCIe traddr=0000.'${name}' ns=1'

run_spdk_nvme_fio --filename="$filename" "$@"
