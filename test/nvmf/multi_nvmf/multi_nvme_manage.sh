#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter multi_process_nvmf_and_nvme_manage

cp $rootdir/test/nvmf/nvmf.conf $testdir/nvmf.conf.tmp

# Start the iSCSI target without using stub
$NVMF_APP -c $testdir/nvmf.conf.tmp &
nvmfpid=$!
echo "NVMF target launched. pid: $nvmfpid"
trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $nvmfpid 5260
echo "NVMF target has started."

nvme_count=0
target_name_count=0
for bdf in $(linux_iter_pci 0108); do
	$rpc_py construct_nvme_bdev -b "Nvme${nvme_count}" -t "pcie" -a "${bdf}"
	let nvme_count+=1
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode${nvme_count} "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001 -n Nvme${target_name_count}n1
	let target_name_count+=1
done
echo "NVMf subsystem created."
sleep 1

echo "Running nvme_manage"
echo 8|$rootdir/examples/nvme/nvme_manage/nvme_manage -i 0

trap - SIGINT SIGTERM EXIT

rm -f $testdir/nvmf.conf.tmp
cnode_count=$nvme_count
for((i=1; i<=$cnode_count; i++)); do
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i}
done
killprocess $nvmfpid
timing_exit multi_process_nvmf_nvme_manage
