#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter reset

# iSCSI target configuration
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/iscsi_fio.py"

if ! hash sg_reset; then
	exit 1
fi

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT
sleep 1
dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

sleep 1
dev_list=$(get_devices_list iscsi)
$fio_py $dev_list 512 1 read 60 &
fiopid=$!
echo "FIO pid: $fiopid"

trap "iscsicleanup; killprocess $pid; killprocess $fiopid; exit 1" SIGINT SIGTERM EXIT

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
timing_exit reset
