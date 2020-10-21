#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $testdir/rpc_iscsi.sock"

BLK_SIZE=4096
RW=randrw
MIX=100
IODEPTH=128
RUNTIME=60
RAMP_TIME=10
FIO_PATH=$CONFIG_FIO_SOURCE_DIR
DISKNO="ALL"
CPUMASK=0x02
NUM_JOBS=1
ISCSI_TGT_CM=0x02

# Performance test for iscsi_tgt, run on devices with proper hardware support (target and inititator)
function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Usage: $(basename $1) [options]"
	echo "-h, --help                Print help and exit"
	echo "    --fiopath=PATH        Path to fio directory on initiator. [default=$FIO_PATH]"
	echo "    --disk_no=INT,ALL     Number of disks to test on, if =ALL then test on all found disks. [default=$DISKNO]"
	echo "    --target_ip=IP        The IP address of target used for test."
	echo "    --initiator_ip=IP     The IP address of initiator used for test."
	echo "    --init_mgmnt_ip=IP    The IP address of initiator used for communication."
	echo "    --iscsi_tgt_mask=HEX  iscsi_tgt core mask. [default=$ISCSI_TGT_CM]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help)
					usage $0
					exit 0
					;;
				fiopath=*) FIO_BIN="${OPTARG#*=}" ;;
				disk_no=*) DISKNO="${OPTARG#*=}" ;;
				target_ip=*) TARGET_IP="${OPTARG#*=}" ;;
				initiator_ip=*) INITIATOR_IP="${OPTARG#*=}" ;;
				init_mgmnt_ip=*) IP_I_SSH="${OPTARG#*=}" ;;
				iscsi_tgt_mask=*) ISCSI_TGT_CM="${OPTARG#*=}" ;;
				*)
					usage $0 echo "Invalid argument '$OPTARG'"
					exit 1
					;;
			esac
			;;
		h)
			usage $0
			exit 0
			;;
		*)
			usage $0 "Invalid argument '$optchar'"
			exit 1
			;;
	esac
done

if [ -z "$TARGET_IP" ]; then
	error "No IP address of iscsi target is given"
fi

if [ -z "$INITIATOR_IP" ]; then
	error "No IP address of iscsi initiator is given"
fi

if [ -z "$IP_I_SSH" ]; then
	error "No IP address of initiator is given"
fi

if [ $EUID -ne 0 ]; then
	error "INFO: This script must be run with root privileges"
fi

function ssh_initiator() {
	# shellcheck disable=SC2029
	# (we want to expand $@ immediately, not on the VM)
	ssh -i $HOME/.ssh/spdk_vhost_id_rsa root@$IP_I_SSH "$@"
}

NETMASK=$INITIATOR_IP/32
iscsi_fio_results="$testdir/perf_output/iscsi_fio.json"
rm -rf $iscsi_fio_results
mkdir -p $testdir/perf_output
touch $iscsi_fio_results

timing_enter run_iscsi_app
$SPDK_BIN_DIR/iscsi_tgt -m $ISCSI_TGT_CM -r $testdir/rpc_iscsi.sock --wait-for-rpc &
pid=$!
trap 'rm -f $testdir/perf.job; killprocess $pid; print_backtrace; exit 1' ERR SIGTERM SIGABRT
waitforlisten "$pid" "$testdir/rpc_iscsi.sock"
$rpc_py iscsi_set_options -b "iqn.2016-06.io.spdk" -f "/usr/local/etc/spdk/auth.conf" -o 30 -i -l 0 -a 16
$rpc_py framework_start_init
$rootdir/scripts/gen_nvme.sh | $rpc_py load_subsystem_config
sleep 1
timing_exit run_iscsi_app

timing_enter iscsi_config
bdevs=($($rpc_py bdev_get_bdevs | jq -r '.[].name'))
if [[ $DISKNO == "ALL" ]] || [[ $DISKNO == "all" ]]; then
	DISKNO=${#bdevs[@]}
elif [[ $DISKNO -gt ${#bdevs[@]} ]] || [[ ! $DISKNO =~ ^[0-9]+$ ]]; then
	error "Required device number ($DISKNO) is not a valid number or it's larger than the number of devices found (${#bdevs[@]})"
fi

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

for ((i = 0; i < DISKNO; i++)); do
	$rpc_py iscsi_create_target_node Target${i} Target${i}_alias "${bdevs[i]}:0" "$PORTAL_TAG:$INITIATOR_TAG" 64 -d
done

ssh_initiator "cat > perf.job" < $testdir/perf.job
rm -f $testdir/perf.job
timing_exit iscsi_config

timing_enter iscsi_initiator
ssh_initiator bash -s - $FIO_PATH $TARGET_IP < $testdir/iscsi_initiator.sh
timing_exit iscsi_initiator

ssh_initiator "cat perf_output/iscsi_fio.json" > $iscsi_fio_results
ssh_initiator "rm -rf perf_output perf.job"

killprocess $pid
