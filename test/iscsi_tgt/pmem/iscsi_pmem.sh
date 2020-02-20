#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

BLOCKSIZE=$1
RUNTIME=$2
PMEM_BDEVS=""
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
TGT_NR=10
PMEM_PER_TGT=1
rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_target
"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'iscsicleanup; killprocess $pid; rm -f /tmp/pool_file*; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."
timing_exit start_iscsi_target

timing_enter setup
$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
for i in $(seq 1 $TGT_NR); do
	INITIATOR_TAG=$((i + 1))
	$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

	luns=""
	for j in $(seq 1 $PMEM_PER_TGT); do
		$rpc_py create_pmem_pool /tmp/pool_file${i}_${j} $PMEM_SIZE $PMEM_BLOCK_SIZE
		bdevs_name="$($rpc_py bdev_pmem_create -n pmem${i}_${j} /tmp/pool_file${i}_${j})"
		PMEM_BDEVS+="$bdevs_name "
		luns+="$bdevs_name:$((j - 1)) "
	done
	$rpc_py iscsi_create_target_node Target$i Target${i}_alias "$luns" "1:$INITIATOR_TAG " 256 -d
done
timing_exit setup
sleep 1

timing_enter discovery
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
timing_exit discovery

timing_enter fio_test
$fio_py -p iscsi -i $BLOCKSIZE -d 64 -t randwrite -r $RUNTIME -v
timing_exit fio_test

iscsicleanup

for pmem in $PMEM_BDEVS; do
	$rpc_py bdev_pmem_delete $pmem
done

for i in $(seq 1 $TGT_NR); do
	for c in $(seq 1 $PMEM_PER_TGT); do
		$rpc_py bdev_pmem_delete_pool /tmp/pool_file${i}_${c}
	done
done

trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
rm -f /tmp/pool_file*
killprocess $pid
