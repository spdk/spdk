#!/usr/bin/env bash

set -e

. $(readlink -e "$(dirname $0)/common.sh")
. $SPDK_BUILD_DIR/scripts/common.sh || exit 1
PLUGIN_DIR_NVME=$SPDK_BUILD_DIR/examples/nvme/fio_plugin

trap 'rm -f *.state $nvme_fio_results; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

function run_spdk_nvme_fio(){
	LD_PRELOAD=$PLUGIN_DIR_NVME/fio_plugin $FIO_BIN $BASE_DIR/perf.fio --output-format=json\
	 "$@" --ioengine=$PLUGIN_DIR_NVME/fio_plugin
}

timing_enter config_fio_job
disks=($(iter_pci_class_code 01 08 02))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#disks[@]}
elif [[ $DISKNO -gt ${#disks[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#disks[@]})"
fi

for (( i=0; i < $DISKNO; i++ ))
do
	dev_name='trtype=PCIe traddr='${disks[i]//:/.}' ns=1'
	filename+=$(printf %s":" "$dev_name")
done
timing_exit config_fio_job

timing_enter run_spdk_nvme_fio
run_spdk_nvme_fio --filename="$filename" "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$nvme_fio_results" "--cpumask=$CPUMASK"
timing_exit run_spdk_nvme_fio
