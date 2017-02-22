#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

if ! rdma_nic_available; then
        echo "no NIC for nvmf test"
        exit 0
fi

timing_enter perf

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf -m 0x2 -p 1 -s 512 &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

$rpc_py construct_nvmf_subsystem Virtual nqn.2016-06.io.spdk:cnode1 'transport:RDMA traddr:192.168.100.8 trsvcid:4420' '' -s SPDK00000000000001 -n "$bdevs"
$rpc_py construct_nvmf_subsystem Direct nqn.2016-06.io.spdk:cnode2 'transport:RDMA traddr:192.168.100.8 trsvcid:4420' '' -p "*"

$rootdir/examples/nvme/perf/perf -q 128 -s 4096 -w randrw -M 50 -t 1 -r "trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420"
sync
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode2

trap - SIGINT SIGTERM EXIT

killprocess $nvmfpid
timing_exit perf
