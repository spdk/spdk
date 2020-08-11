#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

function run_fio() {
	local bdev_name=$1
	local iostats
	local start_io_count
	local start_bytes_read
	local end_io_count
	local end_bytes_read
	local run_time=5

	iostats=$($rpc_py bdev_get_iostat -b $bdev_name)
	start_io_count=$(jq -r '.bdevs[0].num_read_ops' <<< "$iostats")
	start_bytes_read=$(jq -r '.bdevs[0].bytes_read' <<< "$iostats")

	$fio_py -p iscsi -i 1024 -d 128 -t randread -r $run_time

	iostats=$($rpc_py bdev_get_iostat -b $bdev_name)
	end_io_count=$(jq -r '.bdevs[0].num_read_ops' <<< "$iostats")
	end_bytes_read=$(jq -r '.bdevs[0].bytes_read' <<< "$iostats")

	IOPS_RESULT=$(((end_io_count - start_io_count) / run_time))
	BANDWIDTH_RESULT=$(((end_bytes_read - start_bytes_read) / run_time))
}

function verify_qos_limits() {
	local result=$1
	local limit=$2

	[ "$(bc <<< "$result > $limit*0.85")" -eq 1 ] \
		&& [ "$(bc <<< "$result < $limit*1.05")" -eq 1 ]
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
IOPS_RESULT=
BANDWIDTH_RESULT=
rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" &
pid=$!
echo "Process pid: $pid"
trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target1 Target1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap 'iscsicleanup; killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

# Run FIO without any QOS limits to determine the raw performance
run_fio Malloc0

# Set IOPS/bandwidth limit to 50% of the actual unrestrained performance.
IOPS_LIMIT=$((IOPS_RESULT / 2))
BANDWIDTH_LIMIT=$((BANDWIDTH_RESULT / 2))
# Set READ bandwidth limit to 50% of the RW bandwidth limit to be able
# to differentiate those two.
READ_BANDWIDTH_LIMIT=$((BANDWIDTH_LIMIT / 2))

# Also round them down to nearest multiple of either 1000 IOPS or 1MB BW
# which are the minimal QoS granularities
IOPS_LIMIT=$((IOPS_LIMIT / 1000 * 1000))
BANDWIDTH_LIMIT_MB=$((BANDWIDTH_LIMIT / 1024 / 1024))
BANDWIDTH_LIMIT=$((BANDWIDTH_LIMIT_MB * 1024 * 1024))
READ_BANDWIDTH_LIMIT_MB=$((READ_BANDWIDTH_LIMIT / 1024 / 1024))
READ_BANDWIDTH_LIMIT=$((READ_BANDWIDTH_LIMIT_MB * 1024 * 1024))

# Limit the I/O rate by RPC, then confirm the observed rate matches.
$rpc_py bdev_set_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
run_fio Malloc0
verify_qos_limits $IOPS_RESULT $IOPS_LIMIT

# Now disable the rate limiting, and confirm the observed rate is not limited anymore.
$rpc_py bdev_set_qos_limit Malloc0 --rw_ios_per_sec 0
run_fio Malloc0
[ "$IOPS_RESULT" -gt "$IOPS_LIMIT" ]

# Limit the I/O rate again.
$rpc_py bdev_set_qos_limit Malloc0 --rw_ios_per_sec $IOPS_LIMIT
run_fio Malloc0
verify_qos_limits $IOPS_RESULT $IOPS_LIMIT

echo "I/O rate limiting tests successful"

# Limit the I/O bandwidth rate by RPC, then confirm the observed rate matches.
$rpc_py bdev_set_qos_limit Malloc0 --rw_ios_per_sec 0 --rw_mbytes_per_sec $BANDWIDTH_LIMIT_MB
run_fio Malloc0
verify_qos_limits $BANDWIDTH_RESULT $BANDWIDTH_LIMIT

# Now disable the bandwidth rate limiting, and confirm the observed rate is not limited anymore.
$rpc_py bdev_set_qos_limit Malloc0 --rw_mbytes_per_sec 0
run_fio Malloc0
[ "$BANDWIDTH_RESULT" -gt "$BANDWIDTH_LIMIT" ]

# Limit the I/O bandwidth rate again with both read/write and read/only.
$rpc_py bdev_set_qos_limit Malloc0 --rw_mbytes_per_sec $BANDWIDTH_LIMIT_MB --r_mbytes_per_sec $READ_BANDWIDTH_LIMIT_MB
run_fio Malloc0
verify_qos_limits $BANDWIDTH_RESULT $READ_BANDWIDTH_LIMIT

echo "I/O bandwidth limiting tests successful"

iscsicleanup
$rpc_py iscsi_delete_target_node 'iqn.2016-06.io.spdk:Target1'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
killprocess $pid

iscsitestfini
