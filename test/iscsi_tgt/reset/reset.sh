#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio-wrapper"

if ! hash sg_reset; then
	exit 1
fi

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

$fio_py -p iscsi -i 512 -d 1 -t read -r 60 &
fiopid=$!
echo "FIO pid: $fiopid"

trap 'iscsicleanup; killprocess $pid; killprocess $fiopid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

# Do 3 resets while making sure iscsi_tgt and fio are still running
for i in 1 2 3; do
	sleep 1
	kill -s 0 $pid
	kill -s 0 $fiopid
	sg_reset -d /dev/$dev
	sleep 1
	kill -s 0 $pid
	kill -s 0 $fiopid
done

kill $fiopid
wait $fiopid || true

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
iscsitestfini
