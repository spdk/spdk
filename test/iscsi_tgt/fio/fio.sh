#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

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

	timing_enter start_iscsi_tgt2

	$ISCSI_APP -c /tmp/iscsi.conf &
	pid=$!
	echo "Process pid: $pid"
	trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $pid
	echo "iscsi_tgt is listening. Running tests..."

	timing_exit start_iscsi_tgt2

	sleep 1
	$fio_py $dev_list 4096 1 randrw 5
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

# iSCSI target configuration
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/iscsi_fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1
dev_list=$(get_devices_list iscsi)

$fio_py $dev_list 4096 1 randrw 1 verify
$fio_py $dev_list 131072 32 randrw 1 verify
$fio_py $dev_list 524288 128 randrw 1 verify

if [ $RUN_NIGHTLY -eq 0 ]; then
	$fio_py $dev_list 4096 1 write 300 verify

	# Run the running_config test which will generate a config file from the
	#  running iSCSI target, then kill and restart the iSCSI target using the
	#  generated config file
	running_config
fi

# Start hotplug test case.
$fio_py $dev_list 1048576 128 rw 10 &
fio_pid=$!

sleep 3
set +e
$rpc_py delete_bdev 'Malloc0'

wait $fio_pid
fio_status=$?

if [ $fio_status -eq 0 ]; then
       echo "iscsi hotplug test: fio successful - expected failure"
       iscsicleanup
       rm -f $testdir/iscsi.conf
       killprocess $pid
       exit 1
else
       echo "iscsi hotplug test: fio failed as expected"
fi

set -e

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target3'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
iscsicleanup
rm -f $testdir/iscsi.conf
killprocess $pid
#echo 1 > /sys/bus/pci/rescan
#sleep 2
$rootdir/scripts/setup.sh

timing_exit fio
