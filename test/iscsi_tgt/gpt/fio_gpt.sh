#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/iscsi_tgt/common.sh

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

ISCSI_PORT=3260
NVMF_PORT=4420

timing_enter gpt

cp $testdir/iscsi.conf.in $testdir/iscsi.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf

if [ -z "`grep "Nvme0" $testdir/iscsi.conf`" ]; then
	timing_exit gpt
	exit 0
fi

partition_dev $testdir/iscsi.conf NVMe0n1 $rootdir
echo "[Gpt]" >> $testdir/iscsi.conf
echo "  Disable Yes" >> $testdir/iscsi.conf

timing_enter start_iscsi_tgt

# Start the iSCSI target
$ISCSI_APP -c $testdir/iscsi.conf -m 0x1 -p 0 -s 512 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
# The configuration file for the iSCSI target told it to use port 5261 for RPC
waitforlisten $iscsipid 5260
echo "iSCSI target has started."

timing_exit start_iscsi_tgt

echo "Creating an iSCSI target node."
$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py -p add_initiator_group 1 ALL $INITIATOR_IP/32
$rpc_py construct_gpt_bdev Nvme0n1
#since rpc call for gpt is async
sleep 5
$rpc_py construct_target_node Target1 Target1_alias 'Nvme0n1p1:0' '1:2' 64 1 0 0 0
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
trap "iscsicleanup; killprocess $iscsipid; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
sleep 1

echo "Running FIO"
$fio_py 4096 1 randrw 1 verify

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $iscsipid

timing_exit gpt
