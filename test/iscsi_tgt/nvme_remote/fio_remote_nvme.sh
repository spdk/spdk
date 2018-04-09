#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/iscsi_tgt/common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

NVMF_PORT=4420

NVMF_TARGET_IP=10.10.10.1
NVMF_TARGET_INT="nvmf_tgt1"
NVMF_INIT_IP=10.10.10.2
NVMF_INIT_INT="nvmf_init1"

timing_enter nvme_remote

# Create veth (Virtual ethernet) interface pair
ip link add $NVMF_TARGET_INT type veth peer name $NVMF_INIT_INT
ip addr add $NVMF_TARGET_IP/24 dev $NVMF_TARGET_INT
ip link set $NVMF_TARGET_INT up

ip addr add $NVMF_INIT_IP/24 dev $NVMF_INIT_INT
ip link set $NVMF_INIT_INT up

# Initialize rdma on created interfaces
rdma_device_init

# Start the NVMf target
$rootdir/app/nvmf_tgt/nvmf_tgt -c $rootdir/test/nvmf/nvmf.conf -m 0x2 -p 1 -s 512 &
nvmfpid=$!
echo "NVMf target launched. pid: $nvmfpid"
trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $nvmfpid
echo "NVMf target has started."
bdevs=$($rpc_py construct_malloc_bdev 64 512)
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001 -n "$bdevs"
echo "NVMf subsystem created."

timing_enter start_iscsi_tgt

cp $testdir/iscsi.conf $testdir/iscsi.conf.tmp

if [ $1 -eq 1 ]; then
	echo "[NVMe]" >> $testdir/iscsi.conf.tmp
	echo "  TransportID \"trtype:RDMA adrfam:ipv4 traddr:$NVMF_TARGET_IP trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1\" Nvme0" >> $testdir/iscsi.conf.tmp
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
$rpc_py -s "$iscsi_rpc_addr" add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py -s "$iscsi_rpc_addr" add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
if [ $1 -eq 0 ]; then
	$rpc_py -s "$iscsi_rpc_addr" construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
fi
$rpc_py -s "$iscsi_rpc_addr" construct_target_node Target1 Target1_alias 'Nvme0n1:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
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
ip link delete $NVMF_TARGET_INT

report_test_completion "iscsi_nvme_remote"
timing_exit nvme_remote
