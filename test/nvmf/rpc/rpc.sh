#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter rpc
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
timing_exit start_nvmf_tgt

# set times for subsystem construct/delete
if [ $RUN_NIGHTLY -eq 1 ]; then
	times=50
else
	times=3
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

# do frequent add delete.
for i in `seq 1 $times`
do
	j=0
	for bdf in $bdfs; do
		let j=j+1
		$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -s SPDK00000000000001 -n "$bdevs"
	done

	n=$j
	for j in `seq 1 $n`
	do
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j
	done
done

trap - SIGINT SIGTERM EXIT

killprocess $pid
timing_exit rpc
