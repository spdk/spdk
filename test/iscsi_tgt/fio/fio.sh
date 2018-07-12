#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function running_config() {
	# generate a config file from the running iscsi_tgt
	$rpc_py save_config -f /tmp/iscsi.json

	# now start iscsi_tgt again using the generated config file
	# keep the same iscsiadm configuration to confirm that the
	#  config file matched the running configuration
	trap "iscsicleanup; rm -f /tmp/iscsi.json; exit 1" SIGINT SIGTERM EXIT

	timing_enter start_iscsi_tgt2

	$ISCSI_APP -w &
	pid=$!
	echo "Process pid: $pid"
	trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $pid
	$rpc_py load_subsystem_config -f /tmp/iscsi.json
	$rpc_py start_subsystem_init
	echo "iscsi_tgt is listening. Running tests..."
	rm -f /tmp/iscsi.json

	timing_exit start_iscsi_tgt2

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

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -w &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_iscsi_options -o 30 -a 16
$rpc_py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1
$fio_py 4096 1 randrw 1 verify
$fio_py 131072 32 randrw 1 verify
$fio_py 524288 128 randrw 1 verify

if [ $RUN_NIGHTLY -eq 1 ]; then
	$fio_py 4096 1 write 300 verify

	# Run the running_config test which will generate a config file from the
	#  running iSCSI target, then kill and restart the iSCSI target using the
	#  generated config file
	running_config
fi

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target3'

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT
iscsicleanup
killprocess $pid
#echo 1 > /sys/bus/pci/rescan
#sleep 2
$rootdir/scripts/setup.sh

timing_exit fio
