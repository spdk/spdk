#!/usr/bin/env bash

set -e

TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh
source $rootdir/test/nvmf/common.sh

$rootdir/scripts/setup.sh reset
sleep 2
parted -s /dev/nvme0n1 mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'
/usr/sbin/sgdisk -t 1:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nvme0n1 
/usr/sbin/sgdisk -t 2:7c5222bd-8f5d-4087-9c00-bf9843c7b58c /dev/nvme0n1
$rootdir/scripts/setup.sh
sleep 2

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

for bdf in $(linux_iter_pci 0108); do
	echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme0"
	break
done

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

ISCSI_PORT=3260
NVMF_PORT=4420

timing_enter fio_gpt

timing_enter start_iscsi_tgt

cp $testdir/iscsi.conf $testdir/iscsi.conf.tmp

echo "[NVMe]" >> $testdir/iscsi.conf.tmp
echo "  TransportID \"trtype:RDMA adrfam:ipv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1\" Nvme0" >> $testdir/iscsi.conf.tmp

# Start the iSCSI target without using stub
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf.tmp -m 0x1 -p 0 -s 512 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
# The configuration file for the iSCSI target told it to use port 5261 for RPC
waitforlisten $iscsipid 5261
echo "iSCSI target has started."

timing_exit start_iscsi_tgt

sleep 8
echo "Creating an iSCSI target node."
$rpc_py -p 5261 add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py -p 5261 add_initiator_group 1 ALL $INITIATOR_IP/32
$rpc_py -p 5261 construct_nvme_bdev -b "Nvme0" -t "pcie" -f "ipv4" -a "$bdf" -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
$rpc_py -p 5261 construct_target_node Target1 Target1_alias 'Nvme0n1p1:0' '1:1' 64 1 0 0 0
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
trap "iscsicleanup; killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
sleep 1

echo "Running FIO"

$fio_py 262144 64 rw 5 verify
$fio_py 262144 64 write 5 verify
$fio_py 262144 64 randrw 5 verify
$fio_py 262144 64 randwrite 5 verify

iscsicleanup

$rootdir/scripts/setup.sh reset
sleep 3
parted -s /dev/nvme0n1 mklabel msdos
$rootdir/scripts/setup.sh
sleep 1

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

killprocess $iscsipid
rm -f $testdir/iscsi.conf.tmp

timing_exit fio_gpt
