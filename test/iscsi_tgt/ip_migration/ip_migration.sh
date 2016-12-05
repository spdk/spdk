#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

function kill_all_iscsi_target() {
	for ((i=0; i<${#pid[*]}; i++))
	do
		$rpc_py -p $i kill_instance SIGTERM
	done
}

function rpc_config() {
	# $1 = instanceID
	# $2 = Netmask
	$rpc_py -p $1 add_initiator_group 1 ALL $2
	$rpc_py -p $1 construct_malloc_bdev 64 512
}
function rpc_add_ip() {
	$rpc_py -p $1  add_ip_address 1 $2
}

timing_enter ip_migration

# iSCSI target configuration

PORT=3260
RPC_PORT=5260
NETMASK=127.0.0.0/24
MIGRATION_ADDRESS=127.0.0.2
#The iscsi target process parameter array
dpdk_file_prefix=(target1 target2)
instanceID=(0 1)

echo "Running ip migration tests"
exe=./app/iscsi_tgt/iscsi_tgt
for ((i=0; i<${#dpdk_file_prefix[*]}; i++))
do
	$exe -c $testdir/iscsi.conf -i ${instanceID[$i]} -s 1000 &
	pid[$i]=$!
	echo "Process pid: ${pid[$i]}"

	trap "kill_all_iscsi_target; exit 1" SIGINT SIGTERM EXIT

	waitforlisten ${pid[i]} $(expr $RPC_PORT + ${instanceID[$i]})
	echo "iscsi_tgt is listening. Running tests..."
	rpc_config ${instanceID[$i]} $NETMASK
	trap "kill_all_iscsi_target; exit 1" \
		SIGINT SIGTERM EXIT
done

rpc_add_ip ${instanceID[0]} $MIGRATION_ADDRESS
$rpc_py -p ${instanceID[0]} add_portal_group 1 $MIGRATION_ADDRESS:$PORT
$rpc_py -p ${instanceID[0]} construct_target_node ${dpdk_file_prefix[0]} ${dpdk_file_prefix[0]}_alias 'Malloc0:0' '1:1' 64 1 0 0 0

sleep 1
iscsiadm -m discovery -t sendtargets -p $MIGRATION_ADDRESS:$PORT
sleep 1
iscsiadm -m node --login -p $MIGRATION_ADDRESS:$PORT

# fio tests for multi-process
sleep 1
$fio_py 4096 32 randrw 10 &
fiopid=$!
sleep 5

$rpc_py -p 0 kill_instance SIGTERM

rpc_add_ip ${instanceID[1]} $MIGRATION_ADDRESS
$rpc_py -p ${instanceID[1]} add_portal_group 1 $MIGRATION_ADDRESS:$PORT
$rpc_py -p ${instanceID[1]} construct_target_node ${dpdk_file_prefix[0]} ${dpdk_file_prefix[0]}_alias 'Malloc0:0' '1:1' 64 1 0 0 0

wait $fiopid

trap - SIGINT SIGTERM EXIT

iscsicleanup

$rpc_py -p 1 kill_instance SIGTERM
timing_exit ip_migration
