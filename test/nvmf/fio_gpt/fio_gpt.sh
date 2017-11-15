#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

modprobe -v mlx5_ib
modprobe -v nvme-rdma
modprobe -v nvme-fabrics

rpc_py="python $rootdir/scripts/rpc.py"

set -e

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

timing_enter fio_gpt
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}
timing_exit start_nvmf_tgt

$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a "$bdf"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001 -n "Nvme0n1p2"

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

$testdir/nvmf_fio_gpt.py 4096 8 rw 5 verify
$testdir/nvmf_fio_gpt.py 4096 64 write 5 verify
$testdir/nvmf_fio_gpt.py 4096 128 randrw 10 verify
$testdir/nvmf_fio_gpt.py 4096 128 randwrite 10 verify
sync
nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

$rootdir/scripts/setup.sh reset
sleep 3
parted -s /dev/nvme0n1 mklabel msdos
$rootdir/scripts/setup.sh
sleep 1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state
rm -f ./local-job2-2-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fio_gpt
