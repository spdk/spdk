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

timing_enter perf
timing_enter start_nvmf_tgt

cp $testdir/../nvmf.conf $testdir/nvmf.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/nvmf.conf

local_nvme_trid=$(grep TransportID $testdir/nvmf.conf | head -n1 | awk -F"\"" '{print $2}')

$NVMF_APP -c $testdir/nvmf.conf -i 0 &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

if [ -n "$local_nvme_trid" ]; then
	bdevs="$bdevs Nvme0n1"
fi

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -s SPDK00000000000001 -n "$bdevs"

# Test multi-process access to local NVMe device
if [ -n "$local_nvme_trid" ]; then
	$rootdir/examples/nvme/perf/perf -i 0 -q 32 -s 4096 -w randrw -M 50 -t 1 -r "$local_nvme_trid"
fi

$rootdir/examples/nvme/perf/perf -q 32 -s 4096 -w randrw -M 50 -t 1 -r "trtype:RDMA adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420"
sync
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

rm -f $testdir/nvmf.conf

killprocess $nvmfpid
timing_exit perf
