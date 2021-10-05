#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ "$TEST_TRANSPORT" != "rdma" ]; then
	exit 0
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
subsystem="0"
rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0x3

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$subsystem -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$subsystem Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$subsystem -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

"$rootdir/test/dma/test_dma/test_dma" -q 16 -o 4096 -w randrw -M 70 -t 5 -m 0xc --json <(gen_nvmf_target_json $subsystem) -b "Nvme${subsystem}n1"
test_dmapid=$!

wait $test_dmapid
sync

trap - SIGINT SIGTERM EXIT

nvmftestfini
