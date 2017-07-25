#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter multiconnection
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

for i in `seq 1 11`
do
	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i} "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -s SPDK${i} -n "$bdevs"
done

for i in `seq 1 11`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done

$testdir/../fio/nvmf_fio.py 262144 64 read 10
$testdir/../fio/nvmf_fio.py 262144 64 randwrite 10

sync
for i in `seq 1 11`; do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i}
done

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
timing_exit multiconnection
