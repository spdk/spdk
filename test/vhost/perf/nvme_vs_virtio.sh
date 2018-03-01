#!/usr/bin/env bash

set -e

. $(readlink -e "$(dirname $0)/common.sh")

trap 'rm -f *.state $nvme_fio_results $virtio_fio_results; error_exit "\
 ${FUNCNAME}""${LINENO}"' ERR SIGTERM SIGABRT

rm -f $nvme_fio_results $virtio_fio_results

$BASE_DIR/perf_nvme_pmd.sh "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--disk_no=$DISKNO" "--cpumask=$CPUMASK"

$BASE_DIR/perf_virtio_initiator.sh "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--disk_no=$DISKNO" "--cpumask=$CPUMASK"

set +x
results=($(cat $nvme_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.iops'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.bw'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.mean'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."90.000000"'))
results+=($(cat $nvme_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.000000"'))
results+=($(cat $virtio_fio_results | jq -r '.jobs[].read.clat_ns.percentile."99.000000"'))

type=(IOPS BW AVG_LAT p90 p99)
printf "%10s %20s %20s\n" "" "NVMe PMD"     "Virtio"
for i in {0..4}
do
	printf "%10s: %20s %20s\n" ${type[i]} ${results[i * 2]} ${results[i * 2 + 1]}
done
