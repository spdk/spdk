#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

set -e

timing_enter bdev_io_wait
# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./bdev_io_wait.sh iso
nvmftestinit $1
nvmfappstart "-m 0xF --wait-for-rpc"

# Minimal number of bdev io pool (5) and cache (1)
$rpc_py set_bdev_options -p 5 -c 1
$rpc_py start_subsystem_init
$rpc_py nvmf_create_transport -t rdma -u 8192 -p 4

echo -e "$(create_malloc_nvmf_subsystem 1 rdma)" | $rpc_py

echo "[Nvme]" > $testdir/bdevperf.conf
echo "  TransportID \"trtype:rdma adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420\" Nvme0" >> $testdir/bdevperf.conf
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
killprocess $nvmfpid
nvmftestfini $1
timing_exit bdev_io_wait
