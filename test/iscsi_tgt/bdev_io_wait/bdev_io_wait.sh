#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit $1 $2

timing_enter bdev_io_wait

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -m 0x2 -p 1 -s 512 --wait-for-rpc &
pid=$!
echo "iSCSI target launched. pid: $pid"
trap 'killprocess $pid; iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT
waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 4
# Minimal number of bdev io pool (5) and cache (1)
$rpc_py set_bdev_options -p 5 -c 1
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1
trap 'killprocess $pid; rm -f $testdir/bdev.conf; iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT

# Prepare config file for iSCSI initiator
echo "[iSCSI_Initiator]" > $testdir/bdev.conf
echo "  URL iscsi://$TARGET_IP/iqn.2016-06.io.spdk:disk1/0 iSCSI0" >> $testdir/bdev.conf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w write -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w read -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w flush -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w unmap -t 1
rm -f $testdir/bdev.conf

trap - SIGINT SIGTERM EXIT

killprocess $pid

iscsitestfini $1 $2
report_test_completion "bdev_io_wait"
timing_exit bdev_io_wait
