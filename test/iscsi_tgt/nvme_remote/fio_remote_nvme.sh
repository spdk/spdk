#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/iscsi_tgt/common.sh

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

ISCSI_PORT=3260
NVMF_PORT=4420

timing_enter nvme_remote

# Start the NVMf target
$rootdir/app/nvmf_tgt/nvmf_tgt -c $rootdir/test/nvmf/nvmf.conf -m 0x2 -p 1 -s 512 &
nvmfpid=$!
echo "NVMf target launched. pid: $nvmfpid"
trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $nvmfpid
echo "NVMf target has started."
bdevs=$($rpc_py construct_malloc_bdev 64 512)
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001 -n "$bdevs"
echo "NVMf subsystem created."

timing_enter start_iscsi_tgt

cp $testdir/iscsi.conf $testdir/iscsi.conf.tmp

if [ $1 -eq 1 ]; then
	echo "[NVMe]" >> $testdir/iscsi.conf.tmp
	echo "  TransportID \"trtype:RDMA adrfam:ipv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1\" Nvme0" >> $testdir/iscsi.conf.tmp
fi
# Start the iSCSI target without using stub
iscsi_rpc_addr="/var/tmp/spdk-iscsi.sock"
$rootdir/app/iscsi_tgt/iscsi_tgt -r "$iscsi_rpc_addr" -c $testdir/iscsi.conf.tmp -m 0x1 -p 0 -s 512 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $iscsipid "$iscsi_rpc_addr"
echo "iSCSI target has started."

timing_exit start_iscsi_tgt

echo "Creating an iSCSI target node."
$rpc_py -s "$iscsi_rpc_addr" add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py -s "$iscsi_rpc_addr" add_initiator_group 1 ANY $INITIATOR_IP/32
if [ $1 -eq 0 ]; then
	$rpc_py -s "$iscsi_rpc_addr" construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
fi
$rpc_py -s "$iscsi_rpc_addr" construct_target_node Target1 Target1_alias 'Nvme0n1:0' '1:1' 64 1 0 0 0
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
rm -f $testdir/iscsi.conf.tmp
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
killprocess $nvmfpid

report_test_completion "iscsi_nvme_remote"
timing_exit nvme_remote
