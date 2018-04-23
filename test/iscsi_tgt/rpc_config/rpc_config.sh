#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter rpc_config

# $1 = test type (posix/vpp)
if [ "$1" == "posix" ] || [ "$1" == "vpp" ]; then
       TEST_TYPE=$1
else
       echo "No iSCSI test type specified"
       exit 1
fi

MALLOC_BDEV_SIZE=64

rpc_py=$rootdir/scripts/rpc.py
rpc_config_py="python $testdir/rpc_config.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_config_py $rpc_py $TARGET_IP $INITIATOR_IP $ISCSI_PORT $NETMASK $TARGET_NAMESPACE $TEST_TYPE

$rpc_py get_bdevs

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit rpc_config
