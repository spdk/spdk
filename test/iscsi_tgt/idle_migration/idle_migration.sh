#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter idle_migration

# iSCSI target configuration
PORT=3260
RPC_PORT=5260

fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

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
