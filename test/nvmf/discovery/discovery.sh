#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NULL_BDEV_SIZE=102400
NULL_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

if ! hash nvme; then
	echo "nvme command not found; skipping discovery test"
	exit 0
fi

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter discovery

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}

bdevs="$bdevs $($rpc_py construct_null_bdev Null0 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE)"
bdevs="$bdevs $($rpc_py construct_null_bdev Null1 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem Direct nqn.2016-06.io.spdk:cnode1 'transport:RDMA traddr:192.168.100.8 trsvcid:4420' '' -p "*"
$rpc_py construct_nvmf_subsystem Virtual nqn.2016-06.io.spdk:cnode2 'transport:RDMA traddr:192.168.100.8 trsvcid:4420' '' -s SPDK00000000000001 -n "$bdevs"

nvme discover -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "Perform nvmf subsystem discovery via RPC"
$rpc_py get_nvmf_subsystems

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode2

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit discovery
