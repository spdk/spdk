#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

# $1 = test type posix or vpp.
# $2 = "iso" - triggers isolation mode (setting up required environment).
iscsitestinit $2 $1

if [ "$1" == "posix" ] || [ "$1" == "vpp" ]; then
	TEST_TYPE=$1
else
	echo "No iSCSI test type specified"
	exit 1
fi

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
echo "iscsi_tgt is listening. Running tests..."

# RPC framework_wait_init should be already returned, so its process must be non-existed
! ps $rpc_wait_pid

# RPC framework_wait_init will directly returned after subsystem initialized.
$rpc_py framework_wait_init &
rpc_wait_pid=$!
sleep 1
! ps $rpc_wait_pid

timing_exit start_iscsi_tgt

$rpc_config_py $rpc_py $TARGET_IP $INITIATOR_IP $ISCSI_PORT $NETMASK $TARGET_NAMESPACE $TEST_TYPE

$rpc_py bdev_get_bdevs

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid

iscsitestfini $2 $1
