#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter iscsi_lvol

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
if [ $RUN_NIGHTLY -eq 1 ]; then
	NUM_MALLOC=10
	NUM_LVOL=10
else
	NUM_MALLOC=2
	NUM_LVOL=2
fi

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_iscsi_options -o 30 -a 16
$rpc_py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

timing_enter setup
$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
for i in $(seq 1 $NUM_MALLOC); do
	INITIATOR_TAG=$((i + 2))
	$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	# Create a RAID-0 from two malloc bdevs the first time through this loop
	if [ $i -eq 1 ]; then
		# construct 2 malloc bdevs
		# construct RAID bdev and put name of it in $bdev
		malloc_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
		malloc_bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
		$rpc_py construct_raid_bdev -n raid0 -s 64 -r 0 -b "$malloc_bdevs"
		bdev="raid0"
	else
		# construct malloc bdev and put name of it in $bdev
		bdev=$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)
	fi
	ls_guid=$($rpc_py construct_lvol_store $bdev lvs_$i -c 1048576)
	LUNs=""
	for j in $(seq 1 $NUM_LVOL); do
		lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$j 10)
		LUNs+="$lb_name:$((j - 1)) "
	done
	$rpc_py construct_target_node Target$i Target${i}_alias "$LUNs" "1:$INITIATOR_TAG" 256 -d
done
timing_exit setup

sleep 1

timing_enter discovery
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
timing_exit discovery

timing_enter fio
$fio_py 131072 8 randwrite 10 verify
timing_exit fio

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
killprocess $pid
timing_exit iscsi_lvol
