#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

function tgt_init() {
	nvmfappstart -m 0xF

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
}

nvmftestinit
tgt_init

"$rootdir/test/bdev/bdevperf/bdevperf" --json <(gen_nvmf_target_json) -q 128 -o 4096 -w verify -t 1

"$rootdir/test/bdev/bdevperf/bdevperf" --json <(gen_nvmf_target_json) -q 128 -o 4096 -w verify -t 15 -f &
bdevperfpid=$!

sleep 3
kill -9 $nvmfpid

sleep 3
tgt_init

wait $bdevperfpid
sync
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
