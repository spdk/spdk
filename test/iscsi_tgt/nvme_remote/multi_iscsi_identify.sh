#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

# iSCSI target configuration
ISCSI_PORT=3260
TARGET_IP=127.0.0.1

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter multi_process_iscsi_and_identify

cp $testdir/iscsi.conf $testdir/iscsi.conf.tmp

# Start the iSCSI target
$ISCSI_APP -c $testdir/iscsi.conf.tmp -i 0 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $iscsipid 5261
echo "iSCSI target has started."
echo "Creating an iSCSI target node."
$rpc_py -p 5261 add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py -p 5261 add_initiator_group 1 ALL $INITIATOR_IP/32
nvme_count=0
for bdf in $(linux_iter_pci 0108); do
	$rpc_py -p 5261 construct_nvme_bdev -b "Nvme${nvme_count}" -t "pcie" -a "${bdf}"
	let nvme_count+=1
done
for ((i=0,target_name=1; i<$nvme_count; i++,target_name++)); do
	$rpc_py -p 5261 construct_target_node Target${target_name} Target${target_name}_alias Nvme${i}n1:0 '1:1' 64 1 0 0 0
done
sleep 1

echo "Running identify"
$rootdir/examples/nvme/identify/identify -i 0

trap - SIGINT SIGTERM EXIT

rm -f $testdir/iscsi.conf.tmp
cnode_count=($target_name-1)
for ((i=1; i<=$cnode_count; i++)); do
	$rpc_py -p 5261 delete_target_node iqn.2016-06.io.spdk:Target${i}
done
killprocess $iscsipid
timing_exit multi_process_iscsi_and_identify
