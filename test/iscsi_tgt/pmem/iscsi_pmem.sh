#!/usr/bin/env bash

export TARGET_IP=127.0.0.1
export INITIATOR_IP=127.0.0.1

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function clear_pmem_pool()
{
	for pmem in $PMEM_BDEVS; do
		$rpc_py delete_bdev $pmem
	done

	for i in `seq 0 $SUBSYS_NR`; do
		for c in `seq 0 $SUBSYS_NR`; do
			$rpc_py delete_pmem_pool /tmp/pool_file$i$c
		done
	done
}

timing_enter iscsi_pmem

# iSCSI target configuration
PMEM_BDEVS=""
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
SUBSYS_NR=0
rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_pmem

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

trap "iscsicleanup; killprocess $pid; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_pmem

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
for i in `seq 0 $SUBSYS_NR`; do
	INITIATOR_TAG=$((i+2))
    	$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	luns=""
	for c in `seq 0 $SUBSYS_NR`; do
		$rpc_py create_pmem_pool /tmp/pool_file$i$c $PMEM_SIZE $PMEM_BLOCK_SIZE
		bdevs_name="$($rpc_py construct_pmem_bdev /tmp/pool_file$i$c)"
		PMEM_BDEVS+="$bdevs_name "
		luns+="$bdevs_name:$c "
	done
	sleep 0.1
        $rpc_py construct_target_node Target$i Target${i}_alias "$luns" "1:$INITIATOR_TAG " 256 1 0 0 0
done

sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

sleep 1

$fio_py 1048576 64 randwrite 10 verify

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
clear_pmem_pool
iscsicleanup
killprocess $pid
timing_exit iscsi_pmem
