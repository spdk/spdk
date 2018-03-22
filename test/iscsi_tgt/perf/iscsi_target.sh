#!/usr/bin/env bash

set -e

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_PATH="/usr/src/fio"
DISKNO="ALL"
CPUMASK=0x02
. $(readlink -e "$(dirname $0)/../common.sh")

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help              Print help and exit"
	echo "    --bs=INT            The block size in bytes used for I/O units. [default=$BLK_SIZE]"
	echo "    --rw=STR            Type of I/O pattern. [default=$RW]"
	echo "    --rwmixread=INT     Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT       Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --runtime=TIME      Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME    Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiopath=PATH      Path to fio directory. [default=$FIO_PATH]"
	echo "    --disk_no=INT,ALL   Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpumask=HEX       The parameter given is a bit mask of allowed CPUs the job may run on. [default=$CPUMASK]"
	echo "    --target_ip=IP      The IP adress of target used for test."
	echo "    --initiator_ip=IP   The IP adress of initiator used for test."
	echo "    --init_mgmnt_ip=IP  The IP adress of initiator used for communication."
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
			fiopath=*) FIO_BIN="${OPTARG#*=}" ;;
			disk_no=*) DISKNO="${OPTARG#*=}" ;;
			cpumask=*) CPUMASK="${OPTARG#*=}" ;;
			target_ip=*) TARGET_IP="${OPTARG#*=}" ;;
			initiator_ip=*) INITIATOR_IP="${OPTARG#*=}" ;;
			init_mgmnt_ip=*) IP_I_SSH="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

. $(readlink -e "$(dirname $0)/../../common/autotest_common.sh") || exit 1
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

if [ -z "$TARGET_IP" ]; then
	error "No IP adress of iscsi target is given"
fi

if [ -z "$INITIATOR_IP" ]; then
	error "No IP adress of iscsi initiator is given"
fi

if [ -z "$IP_I_SSH" ]; then
	error "No IP adress of initiator is given"
fi

if [[ $EUID -ne 0 ]]; then
	error "INFO: Go away user come back as root"
fi

function ssh_initiator(){
	ssh -i $HOME/.ssh/spdk_vhost_id_rsa root@$IP_I_SSH "$@"
}

NETMASK=$INITIATOR_IP/32
rpc_py="python $rootdir/scripts/rpc.py -s $testdir/rpc_iscsi.sock"
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"
rm -rf $iscsi_fio_results
mkdir -p $testdir/perf_output
touch $iscsi_fio_results

timing_enter run_iscsi_app
$rootdir/app/iscsi_tgt/iscsi_tgt -r $testdir/rpc_iscsi.sock -c $testdir/iscsi.conf &
pid=$!
waitforlisten "$pid" "$testdir/rpc_iscsi.sock"
trap "killprocess $pid; print_backtrace; exit 1" ERR SIGTERM SIGABRT
sleep 1
timing_exit run_iscsi_app

timing_enter iscsi_config
bdevs=($($rpc_py get_bdevs | jq -r '.[].name'))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#bdevs[@]}
elif [[ $DISKNO -gt ${#bdevs[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required devices number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#bdevs[@]})"
fi

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

for (( i=0; i < $DISKNO; i++ ))
do
	$rpc_py construct_target_node Target${i} Target${i}_alias "${bdevs[i]}:0" "$PORTAL_TAG:$INITIATOR_TAG" 64 -d
done

cat $testdir/perf.job | ssh_initiator "cat > perf.job"
timing_exit iscsi_config

timing_enter iscsi_initiator
ssh_initiator bash -s - < $testdir/iscsi_initiator.sh --runtime=$RUNTIME --ramp_time=$RAMP_TIME --bs=$BLK_SIZE\
 --rw=$RW --rwmixread=$MIX --iodepth=$IODEPTH --cpumask=$CPUMASK --ip_tgt_int=$TARGET_IP --ip_init_int=$INITIATOR_IP --fiopath=$FIO_PATH
timing_exit iscsi_initiator

ssh_initiator "cat perf_output/iscsi_fio.json" | cat  > $iscsi_fio_results
ssh_initiator "rm -rf perf_output perf.job"

killprocess $pid
