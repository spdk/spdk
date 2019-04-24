#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function run_fio() {
	local bdev_name=$1
	local iostats=$($rpc_py get_bdevs_iostat -b $bdev_name)
	local run_time=5

	local start_io_count=$(jq -r '.[1].num_read_ops' <<< "$iostats")
	local start_bytes_read=$(jq -r '.[1].bytes_read' <<< "$iostats")

	$fio_py iscsi 1024 128 randread $run_time 1

	iostats=$($rpc_py get_bdevs_iostat -b $bdev_name)
	local end_io_count=$(jq -r '.[1].num_read_ops' <<< "$iostats")
	local end_bytes_read=$(jq -r '.[1].bytes_read' <<< "$iostats")

	IOPS_RESULT=$(((end_io_count-start_io_count)/$run_time))
	BANDWIDTH_RESULT=$(((end_bytes_read-start_bytes_read)/$run_time))
}

function throughput_in_qos_bounds() {
	local result=$1
	local limit=$2

	[ "$(bc <<< "$result > $limit*0.85")" -eq 1 ] && \
	[ "$(bc <<< "$result < $limit*1.05")" -eq 1 ]
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
IOPS_RESULT=
BANDWIDTH_RESULT=
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

# Run FIO without any QOS limits to determine the raw performance
run_fio Malloc0

# Set IOPS/bandwidth limit to some % of the actual unrestrained performance.
# Also round it down to nearest multiple of either 10000 IOPS or 10MB BW
IOPS_LIMIT=$(($IOPS_RESULT/2/10000*10000))
BANDWIDTH_LIMIT=$(($BANDWIDTH_RESULT/2/10485760*10485760))
BANDWIDTH_LIMIT_MB=$(($BANDWIDTH_LIMIT/1024/1024))
READ_BANDWIDTH_LIMIT=$(($BANDWIDTH_LIMIT/2))
READ_BANDWIDTH_LIMIT_MB=$(($READ_BANDWIDTH_LIMIT/1024/1024))

# Limit the I/O rate by RPC, then confirm the observed rate matches.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
run_fio Malloc0
throughput_in_qos_bounds $IOPS_RESULT $IOPS_LIMIT || false

# Now disable the rate limiting, and confirm the observed rate is not limited anymore.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec 0
run_fio Malloc0
[ "$IOPS_RESULT" > "$IOPS_LIMIT" ] || false

# Limit the I/O rate again.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
run_fio Malloc0
throughput_in_qos_bounds $IOPS_RESULT $IOPS_LIMIT || false

echo "I/O rate limiting tests successful"

# Limit the I/O bandwidth rate by RPC, then confirm the observed rate matches.
$rpc_py set_bdev_qos_limit Malloc0 --rw_ios_per_sec 0 --rw_mbytes_per_sec $BANDWIDTH_LIMIT_MB
run_fio Malloc0
throughput_in_qos_bounds $BANDWIDTH_RESULT $BANDWIDTH_LIMIT || false

# Now disable the bandwidth rate limiting, and confirm the observed rate is not limited anymore.
$rpc_py set_bdev_qos_limit Malloc0 --rw_mbytes_per_sec 0
run_fio Malloc0
[ "$BANDWIDTH_RESULT" > "$BANDWIDTH_LIMIT" ] || false

# Limit the I/O bandwidth rate again with both read/write and read/only.
$rpc_py set_bdev_qos_limit Malloc0 --rw_mbytes_per_sec $BANDWIDTH_LIMIT_MB --r_mbytes_per_sec $READ_BANDWIDTH_LIMIT_MB
run_fio Malloc0
throughput_in_qos_bounds $BANDWIDTH_RESULT $READ_BANDWIDTH_LIMIT || false

echo "I/O bandwidth limiting tests successful"

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target1'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
killprocess $pid

timing_exit qos
