#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter initiator

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

# Start the iSCSI target without using stub
# Reason: Two SPDK processes will be started
$ISCSI_APP -m 0x2 -p 1 -s 512 --wait-for-rpc &
pid=$!
echo "iSCSI target launched. pid: $pid"
trap "killprocess $pid;exit 1" SIGINT SIGTERM EXIT
waitforlisten $pid
$rpc_py set_iscsi_options -o 30 -a 4
$rpc_py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1
trap "killprocess $pid; rm -f $testdir/bdev.conf; exit 1" SIGINT SIGTERM EXIT

# Prepare config file for iSCSI initiator
echo "[iSCSI_Initiator]" > $testdir/bdev.conf
echo "  URL iscsi://$TARGET_IP/iqn.2016-06.io.spdk:disk1/0 iSCSI0" >> $testdir/bdev.conf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w verify -t 5 -s 512
if [ $RUN_NIGHTLY -eq 1 ]; then
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w unmap -t 5 -s 512
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w flush -t 5 -s 512
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w reset -t 10 -s 512
fi
rm -f $testdir/bdev.conf

trap - SIGINT SIGTERM EXIT

killprocess $pid

report_test_completion "iscsi_initiator"
timing_exit initiator
