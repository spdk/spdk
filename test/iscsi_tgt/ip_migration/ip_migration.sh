#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

source "$rootdir/test/common/applications.sh"
NETMASK=127.0.0.0/24
MIGRATION_ADDRESS=127.0.0.2

function kill_all_iscsi_target() {
	for ((i = 0; i < 2; i++)); do
		rpc_addr="/var/tmp/spdk${i}.sock"
		$rpc_py -s $rpc_addr spdk_kill_instance SIGTERM
	done
}

function rpc_add_target_node() {
	"${TARGET_NS_CMD[@]}" ip addr add $MIGRATION_ADDRESS/24 dev $TARGET_INTERFACE
	$rpc_py -s $1 iscsi_create_portal_group $PORTAL_TAG $MIGRATION_ADDRESS:$ISCSI_PORT
	$rpc_py -s $1 iscsi_create_target_node target1 target1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
	"${TARGET_NS_CMD[@]}" ip addr del $MIGRATION_ADDRESS/24 dev $TARGET_INTERFACE
}

function iscsi_tgt_start() {
	# $1 = RPC server address
	# $2 = Core Mask

	"${ISCSI_APP[@]}" -r $1 -m $2 --wait-for-rpc &
	pid=$!
	echo "Process pid: $pid"

	trap 'kill_all_iscsi_target; exit 1' SIGINT SIGTERM EXIT

	waitforlisten $pid $1
	$rpc_py -s $1 iscsi_set_options -o 30 -a 64
	$rpc_py -s $1 framework_start_init
	echo "iscsi_tgt is listening. Running tests..."

	$rpc_py -s $1 iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	$rpc_py -s $1 bdev_malloc_create 64 512

	trap 'kill_all_iscsi_target;  iscsitestfini; exit 1' SIGINT SIGTERM EXIT
}

echo "Running ip migration tests"
timing_enter start_iscsi_tgt_0
rpc_first_addr="/var/tmp/spdk0.sock"
iscsi_tgt_start $rpc_first_addr 1
timing_exit start_iscsi_tgt_0

timing_enter start_iscsi_tgt_1
rpc_second_addr="/var/tmp/spdk1.sock"
iscsi_tgt_start $rpc_second_addr 2
timing_exit start_iscsi_tgt_1

rpc_add_target_node $rpc_first_addr

sleep 1
iscsiadm -m discovery -t sendtargets -p $MIGRATION_ADDRESS:$ISCSI_PORT
sleep 1
iscsiadm -m node --login -p $MIGRATION_ADDRESS:$ISCSI_PORT
waitforiscsidevices 1

# fio tests for multi-process
$fio_py -p iscsi -i 4096 -d 32 -t randrw -r 12 &
fiopid=$!
sleep 3

$rpc_py -s $rpc_first_addr spdk_kill_instance SIGTERM

rpc_add_target_node $rpc_second_addr

wait $fiopid

trap - SIGINT SIGTERM EXIT

iscsicleanup

$rpc_py -s $rpc_second_addr spdk_kill_instance SIGTERM
iscsitestfini
