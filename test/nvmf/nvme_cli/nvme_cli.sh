#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter nvme_cli
timing_enter start_nvmf_tgt
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -s SPDK00000000000001 -n "$bdevs"

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

nvme list

for ctrl in /dev/nvme?; do
	nvme id-ctrl $ctrl
	nvme smart-log $ctrl
done

for ns in /dev/nvme?n*; do
	nvme id-ns $ns
done

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true
nvme disconnect -n "nqn.2016-06.io.spdk:cnode2" || true

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit nvme_cli
