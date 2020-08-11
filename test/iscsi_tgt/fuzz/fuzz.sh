#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter iscsi_fuzz

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
iscsipid=$!
echo "Process iscsipid: $iscsipid"

trap 'killprocess $iscsipid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $iscsipid
# Fuzz initiator do not support nop-in yet. So we increase nop-in timeout value for now.
$rpc_py iscsi_set_options -o 60 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."
timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
$rpc_py iscsi_create_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1

trap 'killprocess $iscsipid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

$rootdir/test/app/fuzz/iscsi_fuzz/iscsi_fuzz -m 0xF0 -T $TARGET_IP -t 30 2> $output_dir/iscsi_autofuzz_logs.txt

$rpc_py iscsi_delete_target_node 'iqn.2016-06.io.spdk:disk1'

# Delete malloc device
$rpc_py bdev_malloc_delete Malloc0

trap - SIGINT SIGTERM EXIT

killprocess $iscsipid

iscsitestfini

timing_exit iscsi_fuzz
