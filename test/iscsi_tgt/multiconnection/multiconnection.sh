#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
ISCSI_PORT=3260

lvol_bdevs=()

set -e

# Get 1 nvme address through filtering gen_nvme.sh's result
bdfaddr=$($rootdir/scripts/gen_nvme.sh | grep -e '0000:[0-9a-f]\{2\}:[0-9a-f]\{2\}\.[0-9]' -o | head -1)
if [ -z $bdfaddr ]; then
	echo "No nvme device found"
	exit 1
fi

timing_enter multiconnection

timing_enter start_iscsi_tgt
iscsicleanup
# Start the iSCSI target without using stub
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsipid ${RPC_PORT}
timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group 1 ALL $INITIATOR_IP/32

echo "Creating an iSCSI target node."
$rpc_py construct_nvme_bdev -b "nvme0" -t "PCIe" -a "${bdfaddr}"
ls_guid=$($rpc_py construct_lvol_store "nvme0n1" "lvs0" -c 1048576)
LUNs=""
for i in `seq 0 127`; do
	lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd$i 128)
	lvol_bdevs+=("$lb_name")
done
for i in `seq 0 127`; do
	lun0="lvs0/lbd$i:0"
	$rpc_py construct_target_node Target$i Target${i}_alias "$lun0" "1:1" 256 1 0 0 0
done
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
sleep 1

echo "Running FIO"
$fio_py 4096 8 write 5 verify
sync

echo "INFO: Removing lvol bdevs"
for lvol_bdev in "${lvol_bdevs[@]}"; do
	$rpc_py delete_bdev $lvol_bdev
	echo -e "\tINFO: lvol bdev $lvol_bdev removed"
done
sleep 1

echo "INFO: Removing lvol stores"
$rpc_py destroy_lvol_store -l lvs0
echo -e "INFO: lvol store lvs0 removed"

trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
killprocess $iscsipid
timing_exit multiconnection
