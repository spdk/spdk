#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

function running_config() {
	# generate a config file from the running iscsi_tgt
	#  running_config.sh will leave the file at /tmp/iscsi.conf
	$testdir/running_config.sh $pid
	sleep 1

	# now start iscsi_tgt again using the generated config file
	# keep the same iscsiadm configuration to confirm that the
	#  config file matched the running configuration
	killprocess $pid
	trap "iscsicleanup; exit 1" SIGINT SIGTERM EXIT
	./app/iscsi_tgt/iscsi_tgt -c /tmp/iscsi.conf &
	pid=$!
	echo "Process pid: $pid"
	trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $pid ${RPC_PORT}
	echo "iscsi_tgt is listening. Running tests..."

	sleep 1
	$fio_py 4096 1 randrw 5
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter fio

cp $testdir/iscsi.conf.in $testdir/iscsi.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

./app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 64 1 0 0 0
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1
$fio_py 4096 1 randrw 1 verify
$fio_py 131072 32 randrw 1 verify

if [ $RUN_NIGHTLY -eq 1 ]; then
	$fio_py 4096 1 write 300 verify

	# Run the running_config test which will generate a config file from the
	#  running iSCSI target, then kill and restart the iSCSI target using the
	#  generated config file
	running_config
fi

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

iscsicleanup
rm -f $testdir/iscsi.conf
killprocess $pid
timing_exit fio
