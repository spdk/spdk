#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
if [ $RUN_NIGHTLY -eq 1 ]; then
	NUM_LVS=10
	NUM_LVOL=10
else
	NUM_LVS=2
	NUM_LVOL=2
fi

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'iscsicleanup; killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

timing_enter setup
$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
# Create the first LVS from a Raid-0 bdev, which is created from two malloc bdevs
# Create remaining LVSs from a malloc bdev, respectively
for i in $(seq 1 $NUM_LVS); do
	INITIATOR_TAG=$((i + 2))
	$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	if [ $i -eq 1 ]; then
		# construct RAID bdev and put its name in $bdev
		malloc_bdevs="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
		malloc_bdevs+="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
		$rpc_py bdev_raid_create -n raid0 -z 64 -r 0 -b "$malloc_bdevs"
		bdev="raid0"
	else
		# construct malloc bdev and put its name in $bdev
		bdev=$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)
	fi
	ls_guid=$($rpc_py bdev_lvol_create_lvstore $bdev lvs_$i -c 1048576)
	LUNs=""
	for j in $(seq 1 $NUM_LVOL); do
		lb_name=$($rpc_py bdev_lvol_create -u $ls_guid lbd_$j 10)
		LUNs+="$lb_name:$((j - 1)) "
	done
	$rpc_py iscsi_create_target_node Target$i Target${i}_alias "$LUNs" "1:$INITIATOR_TAG" 256 -d
done
timing_exit setup

sleep 1

timing_enter discovery
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices $((NUM_LVS * NUM_LVOL))
timing_exit discovery

timing_enter fio
$fio_py -p iscsi -i 131072 -d 8 -t randwrite -r 10 -v
timing_exit fio

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
killprocess $pid
iscsitestfini
