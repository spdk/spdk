#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
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

timing_enter nmic
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0xF -w &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_nvmf_target_options -u 8192 -p 4
$rpc_py start_subsystem_init
timing_exit start_nvmf_tgt

# Create subsystems
bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK1 -n "$bdevs"

set +e
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode2 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK2 -n "$bdevs"
nmic_status=$?

if [ $nmic_status -eq 0 ]; then
	echo " constructing nvmf subsystem passed - failure expected."
	killprocess $pid
	exit 1
else
	echo " constructing nvmf subsystem failed - expected result."
fi
set -e

trap - SIGINT SIGTERM EXIT

killprocess $pid

timing_exit nmic
