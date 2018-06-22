#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function check_qos_works_well() {
	local enable_limit=$1
	local iops_limit=$2
	local retval=0

	start_io_count=$($rpc_py get_bdevs_iostat -b $3 | jq -r '.[1].num_read_ops')
	$fio_py 512 64 randread 5
	end_io_count=$($rpc_py get_bdevs_iostat -b $3 | jq -r '.[1].num_read_ops')

	read_iops=$(((end_io_count-start_io_count)/5))

	if [ $enable_limit = true ]; then
		retval=$(echo "$iops_limit*0.9 < $read_iops && $read_iops < $iops_limit*1.01" | bc)
		if [ $retval -eq 0 ]; then
			echo "Failed to limit the io read rate of malloc bdev by qos"
			exit 1
		fi
	else
		retval=$(echo "$read_iops > $iops_limit" | bc)
		if [ $retval -eq 0 ]; then
			echo "$read_iops less than $iops_limit - expected greater than"
			exit 1
		fi
	fi
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter qos

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
IOPS_LIMIT=20000
rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP &
pid=$!
echo "Process pid: $pid"
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target1 Target1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

# Limit the I/O rate by RPC, then confirm the observed rate matches.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
check_qos_works_well true $IOPS_LIMIT Malloc0

# Now disable the rate limiting, and confirm the observed rate is not limited anymore.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec 0
check_qos_works_well false $IOPS_LIMIT Malloc0

# Limit the I/O rate again.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
check_qos_works_well true $IOPS_LIMIT Malloc0
echo "I/O rate limiting tests successful"

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target1'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
killprocess $pid

timing_exit qos
