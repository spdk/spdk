#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
ISCSI_PORT=3260

LVOL_BDEV_SIZE=128
CONNECTION_NUMBER=39
lvol_bdevs=()

# Remove lvol bdevs and stores
function remove_backends()
{
	echo "INFO: Removing lvol bdevs"
	for i in `seq 0 $CONNECTION_NUMBER`; do
		lun0="lvs0/lbd_$i"
		$rpc_py delete_bdev $lun0
		echo -e "\tINFO: lvol bdev $lun0 removed"
	done
	sleep 1

	echo "INFO: Removing lvol stores"
	$rpc_py destroy_lvol_store -l lvs0
	echo "INFO: lvol store lvs0 removed"

	return 0
}

set -e

# Get 1 nvme info through filtering gen_nvme.sh's result.
# Only need one nvme and discard the remaining lines from the 16th line.
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf
sed -i '16,$d' $testdir/iscsi.conf

timing_enter multiconnection
timing_enter start_iscsi_tgt
iscsicleanup

# Start the iSCSI target without using stub
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "remove_backends; iscsicleanup; killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsipid ${RPC_PORT}
timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group 1 ALL $INITIATOR_IP/32

echo "Creating an iSCSI target node."
ls_guid=$($rpc_py construct_lvol_store "Nvme0n1" "lvs0" -c 1048576)
for i in `seq 0 $CONNECTION_NUMBER`; do
	lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$i $LVOL_BDEV_SIZE)
	lvol_bdevs+=("$lb_name")
done
for i in `seq 0 $CONNECTION_NUMBER`; do
	lun0="lvs0/lbd_$i:0"
	$rpc_py construct_target_node Target$i Target${i}_alias "$lun0" "1:1" 256 1 0 0 0
done
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
sleep 1

echo "Running FIO"
$fio_py 131072 16 randwrite 10
sync

trap - SIGINT SIGTERM EXIT

# Remove added nvme information, restored to the original conf.
# Discard the remaining lines from the 14th line.
sed -i '14,$d' $testdir/iscsi.conf

rm -f ./local-job*
iscsicleanup
killprocess $iscsipid
timing_exit multiconnection
