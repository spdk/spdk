#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter bdev_io_wait
nvmftestinit
nvmfappstart "-m 0xF --wait-for-rpc"

# Minimal number of bdev io pool (5) and cache (1)
$rpc_py set_bdev_options -p 5 -c 1
$rpc_py start_subsystem_init
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener  nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "[Nvme]" > $testdir/bdevperf.conf
echo "  TransportID \"trtype:$TEST_TRANSPORT adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT\" Nvme0" >> $testdir/bdevperf.conf
$rootdir/test/bdev/bdevperf/bdevperf -m 0x10 -i 1 -c $testdir/bdevperf.conf -q 128 -o 4096 -w write -t 1 &
WRITE_PID=$!
$rootdir/test/bdev/bdevperf/bdevperf -m 0x20 -i 2 -c $testdir/bdevperf.conf -q 128 -o 4096 -w read -t 1 &
READ_PID=$!
$rootdir/test/bdev/bdevperf/bdevperf -m 0x40 -i 3 -c $testdir/bdevperf.conf -q 128 -o 4096 -w flush -t 1  &
FLUSH_PID=$!
$rootdir/test/bdev/bdevperf/bdevperf -m 0x80 -i 4 -c $testdir/bdevperf.conf -q 128 -o 4096 -w unmap -t 1 &
UNMAP_PID=$!
sync

wait $WRITE_PID
wait $READ_PID
wait $FLUSH_PID
wait $UNMAP_PID

rm -rf $testdir/bdevperf.conf
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit bdev_io_wait
