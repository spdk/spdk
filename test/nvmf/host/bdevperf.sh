#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

timing_enter bdevperf
timing_enter start_nvmf_tgt

$NVMF_APP -m 0xF &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
timing_exit start_nvmf_tgt

$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "[Nvme]" > $testdir/bdevperf.conf
echo "  TransportID \"trtype:$TEST_TRANSPORT adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT\" Nvme0" >> $testdir/bdevperf.conf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w verify -t 1
sync
rm -rf $testdir/bdevperf.conf
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
timing_exit bdevperf
