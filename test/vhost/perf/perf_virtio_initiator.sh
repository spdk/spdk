#!/usr/bin/env bash

set -e

. $(readlink -e "$(dirname $0)/common.sh")
PLUGIN_DIR_BDEV=$SPDK_BUILD_DIR/examples/bdev/fio_plugin

trap 'rm -f *.state $virtio_fio_results $BASE_DIR/bdev.conf; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

RPC_PY="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

function run_spdk_virtio_fio() {
	LD_PRELOAD=$PLUGIN_DIR_BDEV/fio_plugin $FIO_BIN $BASE_DIR/perf.fio --ioengine=spdk_bdev\
	 --spdk_conf=$BASE_DIR/bdev.conf "$@" --spdk_mem=1024\
	 --output-format=json
}

timing_enter spdk_vhost_run
spdk_vhost_run --conf-path=$BASE_DIR
timing_exit spdk_vhost_run

timing_enter config_fio_job
bdevs=($(jq -r '.[].name' <<<  $($RPC_PY get_bdevs)))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#bdevs[@]}
elif [[ $DISKNO -gt ${#bdevs[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#bdevs[@]})"
fi

touch $BASE_DIR/bdev.conf
for (( i=0; i < $DISKNO; i++ ))
do
	$RPC_PY construct_vhost_scsi_controller naa.${bdevs[i]}.0
	$RPC_PY add_vhost_scsi_lun naa.${bdevs[i]}.0 0 ${bdevs[i]}
	echo "[VirtioUser$i]" >> $BASE_DIR/bdev.conf
	echo "  Path naa.${bdevs[i]}.0" >> $BASE_DIR/bdev.conf
	echo "  Queues 2" >> $BASE_DIR/bdev.conf
	echo "" >> $BASE_DIR/bdev.conf
done
timing_exit config_fio_job

vbdevs=$(discover_bdevs $SPDK_BUILD_DIR $BASE_DIR/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

timing_enter run_spdk_virtio_fio
run_spdk_virtio_fio  --filename=$virtio_bdevs  "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$virtio_fio_results" "--cpumask=$CPUMASK"
timing_exit run_spdk_virtio_fio

rm -f $BASE_DIR/bdev.conf
timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill
