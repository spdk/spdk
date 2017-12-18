#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

PORT=3260
NETMASK=127.0.0.0/24
MIGRATION_ADDRESS=127.0.0.2

function kill_all_iscsi_target() {
	for ((i=0; i<2; i++))
	do
		rpc_addr="/var/tmp/spdk${i}.sock"
		$rpc_py -s $rpc_addr kill_instance SIGTERM
	done
}

function rpc_config() {
	# $1 = RPC server address
	# $2 = Netmask
	$rpc_py -s $1 add_initiator_group 1 ANY $2
	$rpc_py -s $1 construct_malloc_bdev 64 512
}
function rpc_add_ip() {
	$rpc_py -s $1  add_ip_address 1 $2
}

timing_enter ip_migration

# iSCSI target configuration

echo "Running ip migration tests"
for ((i=0; i<2; i++))
do
	timing_enter start_iscsi_tgt_$i

	rpc_addr="/var/tmp/spdk${i}.sock"

	# TODO: run the different iSCSI instances on non-overlapping CPU masks
	$ISCSI_APP -r $rpc_addr -c $testdir/iscsi.conf -s 1000 -i $i -m $ISCSI_TEST_CORE_MASK &
	pid=$!
	echo "Process pid: $pid"

	trap "kill_all_iscsi_target; exit 1" SIGINT SIGTERM EXIT

	waitforlisten $pid $rpc_addr
	echo "iscsi_tgt is listening. Running tests..."

	timing_exit start_iscsi_tgt_$i

	rpc_config $rpc_addr $NETMASK
	trap "kill_all_iscsi_target; exit 1" \
		SIGINT SIGTERM EXIT
done

rpc_first_addr="/var/tmp/spdk0.sock"
rpc_add_ip $rpc_first_addr $MIGRATION_ADDRESS
$rpc_py -s $rpc_first_addr add_portal_group 1 $MIGRATION_ADDRESS:$PORT
$rpc_py -s $rpc_first_addr construct_target_node target1 target1_alias 'Malloc0:0' '1:1' 64 1 0 0 0

sleep 1
iscsiadm -m discovery -t sendtargets -p $MIGRATION_ADDRESS:$PORT
sleep 1
iscsiadm -m node --login -p $MIGRATION_ADDRESS:$PORT

# fio tests for multi-process
sleep 1
$fio_py 4096 32 randrw 10 &
fiopid=$!
sleep 5

$rpc_py -s $rpc_first_addr kill_instance SIGTERM

rpc_second_addr="/var/tmp/spdk1.sock"
rpc_add_ip $rpc_second_addr $MIGRATION_ADDRESS
$rpc_py -s $rpc_second_addr add_portal_group 1 $MIGRATION_ADDRESS:$PORT
$rpc_py -s $rpc_second_addr construct_target_node target1 target1_alias 'Malloc0:0' '1:1' 64 1 0 0 0

wait $fiopid

trap - SIGINT SIGTERM EXIT

iscsicleanup

$rpc_py -s $rpc_second_addr kill_instance SIGTERM
report_test_completion "nightly_iscsi_ip_migration"
timing_exit ip_migration
