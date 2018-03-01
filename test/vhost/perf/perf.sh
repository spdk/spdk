#!/usr/bin/env bash

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

nvme_fio_results="$BASE_DIR/nvme_fio"
virtio_fio_results="$BASE_DIR/virtio_fio"
source $COMMON_DIR/common.sh
PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
FIO_BIN="/usr/src/fio/fio"
virtio_bdevs=""
results=""
BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=5
IOENGINE=$PLUGIN_DIR/fio_plugin

trap 'rm -f *.state $ROOT_DIR/spdk.tar.gz; error_exit "${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

function run_spdk_fio() {
	LD_PRELOAD=$PLUGIN_DIR/fio_plugin $FIO_BIN --ioengine=spdk_bdev\
         "$@" --spdk_mem=1024
}

function run_spdk_nvme_fio(){
	fio $BASE_DIR/perf.job -output=$nvme_fio_results -output-format=json --filename="$filename" -ioengine=$IOENGINE
}

name=$(lspci | grep -i Non | awk '{print $1}')
name=${name//[:]/.}
filename='trtype=PCIe traddr=0000.'${name}' ns=1'

run_spdk_nvme_fio
spdk_vhost_run --conf-path=$BASE_DIR

$RPC_PY construct_vhost_scsi_controller naa.Nvme0n1
$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

run_spdk_fio $BASE_DIR/perf.fio --filename=$virtio_bdevs --spdk_conf=$BASE_DIR/bdev.conf \
	 -output=$virtio_fio_results -output-format=json

rm -f *.state $ROOT_DIR/spdk.tar.gz
spdk_vhost_kill

results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.900000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.900000"'))

set +x
type=(IOPS BW MEAN p90 p99)
printf "%10s %20s %20s\n" "" "NVMe PMD"     "Virtio"
for i in {0..4}
do
	printf "%10s: %20s %20s\n" ${type[i]} ${results[$i*2]} ${results[$i*2+1]}
done
