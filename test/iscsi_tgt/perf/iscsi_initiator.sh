#!/usr/bin/env bash

set -xe
trap "exit 1" ERR SIGTERM SIGABRT

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			bs=*) BLK_SIZE="${OPTARG#*=}" ;;
			rw=*) RW="${OPTARG#*=}" ;;
			rwmixread=*) MIX="${OPTARG#*=}" ;;
			iodepth=*) IODEPTH="${OPTARG#*=}" ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramp_time=*) RAMP_TIME="${OPTARG#*=}" ;;
			fiobin=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) DISKNO="${OPTARG#*=}" ;;
			cpumask=*) CPUMASK="${OPTARG#*=}" ;;
			ip_target=*) IP_T="${OPTARG#*=}" ;;
			ip_initiator=*) IP_I="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

testdir=$(readlink -f $(dirname $0))
ISCSI_PORT=3260

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

function run_spdk_iscsi_fio(){
	$FIO_BIN $testdir/perf.job "$@" --output-format=json
}

mkdir -p $testdir/perf_output
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"
trap "iscsiadm -m node --logout; iscsiadm -m node -o delete; exit 1" ERR SIGTERM SIGABRT
iscsiadm -m discovery -t sendtargets -p $IP_T:$ISCSI_PORT
iscsiadm -m node --login -p $IP_T:$ISCSI_PORT
sleep 1

disks=($(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'))
for (( i=0; i < ${#disks[@]}; i++ ))
do
	filename+=$(printf /dev/%s: "${disks[i]}")
done

run_spdk_iscsi_fio --filename=$filename "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$iscsi_fio_results" "--cpumask=$CPUMASK"

iscsiadm -m node --logout || true
iscsiadm -m node -o delete || true
