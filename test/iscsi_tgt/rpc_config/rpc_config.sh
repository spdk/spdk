#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter rpc_config

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64


rpc_py=$rootdir/scripts/rpc.py
rpc_config_py="python $testdir/rpc_config.py"


./app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_config_py $rpc_py

$rpc_py get_bdevs

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit rpc_config
