#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xF --wait-for-rpc

# Minimal number of bdev io pool (5) and cache (1)
$rpc_py bdev_set_options -p 5 -c 1
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

"$rootdir/test/bdev/bdevperf/bdevperf" -m 0x10 -i 1 --json <(gen_nvmf_target_json) -q 128 -o 4096 -w write -t 1 &
WRITE_PID=$!
"$rootdir/test/bdev/bdevperf/bdevperf" -m 0x20 -i 2 --json <(gen_nvmf_target_json) -q 128 -o 4096 -w read -t 1 &
READ_PID=$!
"$rootdir/test/bdev/bdevperf/bdevperf" -m 0x40 -i 3 --json <(gen_nvmf_target_json) -q 128 -o 4096 -w flush -t 1 &
FLUSH_PID=$!
"$rootdir/test/bdev/bdevperf/bdevperf" -m 0x80 -i 4 --json <(gen_nvmf_target_json) -q 128 -o 4096 -w unmap -t 1 &
UNMAP_PID=$!
sync

wait $WRITE_PID
wait $READ_PID
wait $FLUSH_PID
wait $UNMAP_PID

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
