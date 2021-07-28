#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

MALLOC_BDEV_SIZE=64

rpc_py=$rootdir/scripts/rpc.py
rpc_config_py="$testdir/rpc_config.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py framework_wait_init &
rpc_wait_pid=$!
$rpc_py iscsi_set_options -o 30 -a 16

# RPC framework_wait_init should be blocked, so its process must be existed
ps $rpc_wait_pid

$rpc_py framework_start_init
sleep 1
echo "iscsi_tgt is listening. Running tests..."

# RPC framework_wait_init should be already returned, so its process must be non-existed
NOT ps $rpc_wait_pid

# RPC framework_wait_init will directly returned after subsystem initialized.
$rpc_py framework_wait_init &
rpc_wait_pid=$!
sleep 1
NOT ps $rpc_wait_pid

timing_exit start_iscsi_tgt

$rpc_config_py $rpc_py $TARGET_IP $INITIATOR_IP $ISCSI_PORT $NETMASK $TARGET_NAMESPACE

$rpc_py bdev_get_bdevs

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid

iscsitestfini
