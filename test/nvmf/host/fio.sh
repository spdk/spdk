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

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 0
fi

timing_enter fio
timing_enter start_nvmf_tgt

$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev 64 512)"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -s SPDK00000000000001 -n "$bdevs"

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

LD_PRELOAD=$PLUGIN_DIR/fio_plugin /usr/src/fio/fio $PLUGIN_DIR/example_config.fio --filename="trtype=RDMA adrfam=IPv4 traddr=$NVMF_FIRST_TARGET_IP trsvcid=4420 ns=1"

sync

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

killprocess $nvmfpid
timing_exit fio
