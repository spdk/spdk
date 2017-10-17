#!/usr/bin/env bash

export TARGET_IP=127.0.0.1
export INITIATOR_IP=127.0.0.1

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter iscsi_lvol

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
for i in `seq 0 9`; do
    INITIATOR_TAG=$((i+2))
    $rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
    bdev=$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)
    ls_guid=$($rpc_py construct_lvol_store $bdev -c 1048576)
    LUNs=""
    for j in `seq 0 9`; do
        lb_guid=$($rpc_py construct_lvol_bdev $ls_guid 10)
        LUNs+="$lb_guid:$j "
    done
    $rpc_py construct_target_node Target$i Target${i}_alias "$LUNs" "1:$INITIATOR_TAG" 256 1 0 0 0
done
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

$fio_py 131072 8 randwrite 10 verify

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
killprocess $pid
timing_exit iscsi_lvol
