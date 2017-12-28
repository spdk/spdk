#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

# iSCSI target configuration
BLOCKSIZE=$1
RUNTIME=$2
PMEM_BDEVS=""
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
TGT_NR=10
PMEM_PER_TGT=1
rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter iscsi_pmem

timing_enter start_iscsi_target
$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

trap "iscsicleanup; killprocess $pid; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."
timing_exit start_iscsi_target

timing_enter setup
$rpc_py add_portal_group 1 $TARGET_IP:$PORT
for i in `seq 1 $TGT_NR`; do
	INITIATOR_TAG=$((i+1))
	$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

	luns=""
	for j in `seq 1 $PMEM_PER_TGT`; do
		$rpc_py create_pmem_pool /tmp/pool_file${i}_${j} $PMEM_SIZE $PMEM_BLOCK_SIZE
		bdevs_name="$($rpc_py construct_pmem_bdev -n pmem${i}_${j} /tmp/pool_file${i}_${j})"
		PMEM_BDEVS+="$bdevs_name "
		luns+="$bdevs_name:$((j-1)) "
	done
	$rpc_py construct_target_node Target$i Target${i}_alias "$luns" "1:$INITIATOR_TAG " 256 1 0 0 0
done
timing_exit setup
sleep 1

timing_enter discovery
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT
timing_exit discovery

timing_enter fio_test
$fio_py $BLOCKSIZE 64 randwrite $RUNTIME verify
timing_exit fio_test

iscsicleanup

for pmem in $PMEM_BDEVS; do
	$rpc_py delete_bdev $pmem
done

for i in `seq 1 $TGT_NR`; do
	for c in `seq 1 $PMEM_PER_TGT`; do
		$rpc_py delete_pmem_pool /tmp/pool_file${i}_${c}
	done
done

trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
rm -f /tmp/pool_file*
killprocess $pid
timing_exit iscsi_pmem
