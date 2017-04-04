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

timing_enter idle_migration

# iSCSI target configuration
PORT=3260
RPC_PORT=5260

fio_py="python $rootdir/scripts/fio.py"

./app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$testdir/build_configuration.sh

sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 5

# verify that ids has connections in idle state
python $testdir/connection_status.py idle

# start fio in background - while it is running, verify that connections are active
$fio_py 4096 16 randrw 15 &
fiopid=$!
sleep 5
python $testdir/connection_status.py active
kill $fiopid
wait $fiopid || true
sleep 1

# verify again that ids has connections in idle state
python $testdir/connection_status.py idle

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit idle_migration
