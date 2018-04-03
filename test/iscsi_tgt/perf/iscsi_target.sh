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
NUM_JOBS=1
ISCSI_TGT_CM=0x02
USE_VPP=false
. $(readlink -e "$(dirname $0)/../common.sh")

# Performance test for iscsi_tgt, run on devices with proper hardware support (target and inititator)
function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --bs=INT              The block size in bytes used for I/O units. [default=$BLK_SIZE]"
	echo "    --rw=STR              Type of I/O pattern. [default=$RW]"
	echo "    --rwmixread=INT       Percentage of a mixed workload that should be reads. [default=$MIX]"
	echo "    --iodepth=INT         Number of I/O units to keep in flight against the file. [default=$IODEPTH]"
	echo "    --runtime=TIME        Tell fio to terminate processing after the specified period of time. [default=$RUNTIME]"
	echo "    --ramp_time=TIME      Fio will run the specified workload for this amount of time before logging any performance numbers. [default=$RAMP_TIME]"
	echo "    --fiopath=PATH        Path to fio directory on initiator. [default=$FIO_PATH]"
	echo "    --disk_no=INT,ALL     Number of disks to test on, if =ALL then test on all found disk. [default=$DISKNO]"
	echo "    --cpumask=HEX         The parameter given is a bit mask of allowed CPUs the job may run on. [default=$CPUMASK]"
	echo "    --numjobs=INT         Create the specified number of clones of this job. Each clone of job is spawned as an independent thread or process. [default=$NUM_JOBS]"
	echo "    --target_ip=IP        The IP adress of target used for test."
	echo "    --initiator_ip=IP     The IP adress of initiator used for test."
	echo "    --init_mgmnt_ip=IP    The IP adress of initiator used for communication."
	echo "    --iscsi_tgt_mask=HEX  iscsi_tgt core mask. [default=$ISCSI_TGT_CM]"
	echo "    --with-vpp            Test iscsi_tgt with vpp enabled."
	echo "    --dpdk_drv=STR        If vpp is enabled: name of the DPDK driver to bind to target NIC."
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
			numjobs=*) NUM_JOBS="${OPTARG#*=}" ;;
			target_ip=*) TARGET_IP="${OPTARG#*=}" ;;
			initiator_ip=*) INITIATOR_IP="${OPTARG#*=}" ;;
			init_mgmnt_ip=*) IP_I_SSH="${OPTARG#*=}" ;;
			iscsi_tgt_mask=*) ISCSI_TGT_CM="${OPTARG#*=}" ;;
			with-vpp) USE_VPP=true ;;
			dpdk_drv=*) DPDK_DRV="${OPTARG#*=}" ;;
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
trap "print_backtrace; exit 1" ERR SIGTERM SIGABRT
TGT_NETMASK=$(ip a | grep $TARGET_IP | awk '{print $2}')
TGT_INT=$(ip a | grep $TARGET_IP | awk '{print $NF}')
TGT_INT_ADDR=$($rootdir/dpdk/usertools/dpdk-devbind.py --status | grep $TGT_INT | awk '{print $1}')
TGT_INT_DRV=$($rootdir/dpdk/usertools/dpdk-devbind.py --status | grep -oP "(?<=$TGT_INT drv=).*" | awk '{print $1}')

if [ -z "$TARGET_IP" ]; then
	error "No IP adress of iscsi target is given"
fi

if [ -z "$INITIATOR_IP" ]; then
	error "No IP adress of iscsi initiator is given"
fi

if [ -z "$IP_I_SSH" ]; then
	error "No IP adress of initiator is given"
fi

if $USE_VPP && [ -z "$DPDK_DRV" ]; then
	error "No name of DPDK driver is given"
fi

if [[ $EUID -ne 0 ]]; then
	error "INFO: Go away user come back as root"
fi

function ssh_initiator(){
	ssh -i $HOME/.ssh/spdk_vhost_id_rsa root@$IP_I_SSH "$@"
}

function setup_vpp_iscsi()
{
	if ! lsmod | grep $DPDK_DRV; then
		modprobe $DPDK_DRV
	fi

	ip link set dev $TGT_INT down
	sleep 1
	$rootdir/dpdk/usertools/dpdk-devbind.py --bind=$DPDK_DRV $TGT_INT
	sleep 1
	systemctl start vpp
	sleep 7
	vpp_int_name=$(vppctl show int | grep Ethernet | awk '{print $1}')
	vppctl set int ip address $vpp_int_name $TGT_NETMASK
	vppctl set int state $vpp_int_name up
	sleep 1
}

function clean_vpp_iscsi()
{
	sleep 1
	vppctl set int state $vpp_int_name down
	systemctl stop vpp
	sleep 5
	$rootdir/dpdk/usertools/dpdk-devbind.py --bind=$TGT_INT_DRV $TGT_INT_ADDR
	sleep 1
	ip link set dev $TGT_INT up
}

NETMASK=$INITIATOR_IP/32
rpc_py="python $rootdir/scripts/rpc.py -s $testdir/rpc_iscsi.sock"
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"

timing_enter run_iscsi_app
if $USE_VPP; then
	iscsi_fio_results="$testdir/perf_output/iscsi_vpp_fio.json"
	trap "clean_vpp_iscsi; print_backtrace; exit 1" ERR SIGTERM SIGABRT
	setup_vpp_iscsi
fi

$rootdir/app/iscsi_tgt/iscsi_tgt -r $testdir/rpc_iscsi.sock -c $testdir/iscsi.conf -m $ISCSI_TGT_CM &
pid=$!
waitforlisten "$pid" "$testdir/rpc_iscsi.sock"

if $USE_VPP; then
	trap "killprocess $pid; clean_vpp_iscsi; print_backtrace; exit 1" ERR SIGTERM SIGABRT
else
	trap "rm -f $testdir/perf.job; killprocess $pid; print_backtrace; exit 1" ERR SIGTERM SIGABRT
fi

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

cp $testdir/perf.job.in $testdir/perf.job
sed -i "0,/runtime/s@runtime=@runtime=$RUNTIME@" $testdir/perf.job
sed -i "0,/ramp_time/s@ramp_time=@ramp_time=$RAMP_TIME@" $testdir/perf.job
sed -i "0,/bs/s/bs=/bs=$BLK_SIZE/" $testdir/perf.job
sed -i "0,/rw/s@rw=@rw=$RW@" $testdir/perf.job
sed -i "0,/rwmixread/s@rwmixread=@rwmixread=$MIX@" $testdir/perf.job
sed -i "0,/iodepth/s@iodepth=@iodepth=$IODEPTH@" $testdir/perf.job
sed -i "0,/cpumask/s@cpumask=@cpumask=$CPUMASK@" $testdir/perf.job
sed -i "0,/numjobs/s@numjobs=@numjobs=$NUM_JOBS@" $testdir/perf.job

cat $testdir/perf.job | ssh_initiator "cat > perf.job"
rm -f $testdir/perf.job
timing_exit iscsi_config

timing_enter iscsi_initiator
ssh_initiator bash -s - < $testdir/iscsi_initiator.sh $FIO_PATH $TARGET_IP $USE_VPP
timing_exit iscsi_initiator

sleep 3
killprocess $pid

rm -rf $iscsi_fio_results
mkdir -p $testdir/perf_output
touch $iscsi_fio_results
if $USE_VPP; then
	clean_vpp_iscsi
	ssh_initiator "cat perf_output/iscsi_vpp_fio.json" | cat  > $iscsi_fio_results
else
	ssh_initiator "cat perf_output/iscsi_fio.json" | cat  > $iscsi_fio_results
fi

ssh_initiator "rm -rf perf_output perf.job"
