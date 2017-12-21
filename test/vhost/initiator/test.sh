#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

/root/spdk/scripts/setup.sh
source $COMMON_DIR/common.sh

vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

#LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio \
#	--ioengine=spdk_bdev --runtime=10 /root/bdev.fio $fio_job_conf $fio_bdev_conf --spdk_mem=1024
echo $virtio_bdevs
LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio \
	 --ioengine=spdk_bdev --runtime=10 $BASE_DIR/bdev.fio --spdk_conf=$BASE_DIR/bdev.conf --filename=$virtio_bdevs \
	 --io_size=400m --size=100m --spdk_mem=1024
