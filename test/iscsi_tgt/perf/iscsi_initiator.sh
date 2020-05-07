#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
ISCSI_PORT=3260
FIO_PATH=$1
IP_T=$2

set -xe
trap "exit 1" ERR SIGTERM SIGABRT

if [ ! -x $FIO_PATH/fio ]; then
	error "Invalid path of fio binary"
fi

function run_spdk_iscsi_fio() {
	$FIO_PATH/fio $testdir/perf.job "$@" --output-format=json
}

mkdir -p $testdir/perf_output
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"
trap "iscsiadm -m node --logout; iscsiadm -m node -o delete; exit 1" ERR SIGTERM SIGABRT
iscsiadm -m discovery -t sendtargets -p $IP_T:$ISCSI_PORT
iscsiadm -m node --login -p $IP_T:$ISCSI_PORT
waitforiscsidevices 1

disks=($(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'))
for ((i = 0; i < ${#disks[@]}; i++)); do
	filename+=$(printf /dev/%s: "${disks[i]}")
	waitforfile $filename
	echo noop > /sys/block/${disks[i]}/queue/scheduler
	echo "2" > /sys/block/${disks[i]}/queue/nomerges
	echo "1024" > /sys/block/${disks[i]}/queue/nr_requests
done

run_spdk_iscsi_fio --filename=$filename "--output=$iscsi_fio_results"

iscsiadm -m node --logout || true
iscsiadm -m node -o delete || true
