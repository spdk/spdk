#!/usr/bin/env bash

set -e

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_BIN="/usr/src/fio/fio"
DISKNO="ALL"
CPUMASK=0x02

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help            Print help and exit"
	echo "    --bs=INT          The block size in bytes used for I/O units. [default=$BLK_SIZE]"
	echo "    --rw=STR          Type of I/O pattern. [default=$RW]"
	echo "    --rwmixread=INT   Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT     Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --runtime=TIME    Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME  Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiobin=PATH     Path to fio binary. [default=$FIO_BIN]"
	echo "    --disk_no=INT,ALL Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpumask=HEX     The parameter given is a bit mask of allowed CPUs the job may run on. [default=$CPUMASK]"
}

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
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

. $(readlink -e "$(dirname $0)/../../common/autotest_common.sh") || exit 1
. $(readlink -e "$(dirname $0)/../common.sh") || exit 1
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

if [ ! -x $FIO_BIN ]; then
	error "Invalid path of fio binary"
fi

if [[ $EUID -ne 0 ]]; then
	echo "INFO: Go away user come back as root"
	exit 1
fi

function run_spdk_iscsi_fio(){
	$FIO_BIN $testdir/perf.job "$@" --output-format=json
}

mkdir -p $testdir/perf_output
rpc_py="python $rootdir/scripts/rpc.py -s $testdir/rpc_iscsi.sock"
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"
rm -f $iscsi_fio_results

timing_enter run_iscsi_app
$ISCSI_APP -r $testdir/rpc_iscsi.sock -c $testdir/iscsi.conf &
pid=$!
waitforlisten "$pid" "$testdir/rpc_iscsi.sock"
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT
sleep 1
timing_exit run_iscsi_app

timing_enter iscsi_config
bdevs=($($rpc_py get_bdevs | jq -r '.[].name'))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#bdevs[@]}
elif [[ $DISKNO -gt ${#bdevs[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#bdevs[@]})"
fi

$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

for (( i=0; i < $DISKNO; i++ ))
do
	$rpc_py construct_target_node Target${i} Target${i}_alias "${bdevs[i]}:0" '1:2' 64 -d
done

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1
disks=($(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}'))

for (( i=0; i < ${#disks[@]}; i++ ))
do
	filename+=$(printf %s":" "${disks[i]}")
done
timing_exit iscsi_config

timing_enter run_spdk_iscsi_fio
run_spdk_iscsi_fio --filename=$filename "--runtime=$RUNTIME" "--ramp_time=$RAMP_TIME" "--bs=$BLK_SIZE"\
 "--rw=$RW" "--rwmixread=$MIX" "--iodepth=$IODEPTH" "--output=$iscsi_fio_results" "--cpumask=$CPUMASK"
timing_exit run_spdk_iscsi_fio 

trap - SIGINT SIGTERM EXIT
iscsicleanup
killprocess $pid
