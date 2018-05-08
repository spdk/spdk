#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function check_qos_works_well() {
	read_iops=$($fio_py 8192 64 randread 5 | grep "\(read: IOPS=\|write: IOPS=\)" \
		| awk -F, '{print $1}' | awk -F= '{print $2}' | tr -d [k])
	if [ $(echo "18 < $read_iops && $read_iops < 20.1" | bc) -eq 0 ]; then
		echo "read iops error"
		exit 1
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

cp $testdir/iscsi.conf.in $testdir/iscsi.conf
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096
rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf &
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

# After configuring qos for a specified bdev repeately, QoS can still work well.
$rpc_py set_bdev_qos_limit_iops Malloc0 20000
check_qos_works_well
$rpc_py set_bdev_qos_limit_iops Malloc0 0
$fio_py 8192 64 randread 5
$rpc_py set_bdev_qos_limit_iops Malloc0 20000
check_qos_works_well
echo "QoS works well"

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target1'

rm -f ./local-job0-0-verify.state
rm -f $testdir/iscsi.conf
trap - SIGINT SIGTERM EXIT
killprocess $pid

timing_exit qos
