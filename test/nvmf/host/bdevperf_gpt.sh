#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

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

timing_enter bdevperf_gpt

echo "[Nvme]" > $testdir/bdevperf_gpt.conf
echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme0" >> $testdir/bdevperf_gpt.conf
echo "[Rpc]" >> $testdir/bdevperf_gpt.conf
echo "  Enable Yes" >> $testdir/bdevperf_gpt.conf
echo "[Gpt]" >> $testdir/bdevperf_gpt.conf
echo "  Disable No" >> $testdir/bdevperf_gpt.conf

$rootdir/test/lib/bdev/bdevperf/bdevperf -c $testdir/bdevperf_gpt.conf -q 128 -s 4096 -w verify -t 5
sync
rm -rf $testdir/bdevperf_gpt.conf

$rootdir/scripts/setup.sh reset
sleep 3
parted -s /dev/nvme0n1 mklabel msdos
$rootdir/scripts/setup.sh
sleep 1

trap - SIGINT SIGTERM EXIT

timing_exit bdevperf_gpt
