#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

echo $rootdir

source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

timing_enter nvmf_fused_test

echo "[Nvme]" > $testdir/nvmf_fused.conf
echo "  TransportID \"trtype:$TEST_TRANSPORT adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT\" Nvme0" >> $testdir/nvmf_fused.conf

$NVMF_APP -m 0xF &
nvmfpid=$!

gdb_attach $nvmfpid &

trap 'process_shm --id $NVMF_APP_SHM_ID; rm -f $testdir/nvmf_fused.conf; killprocess $nvmfpid; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

$rpc_py bdev_malloc_create -b Malloc0 64 512

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Note that we chose a consistent seed to ensure that this test is consistent in nightly builds.
# gdb -q --batch -ex 'set print thread-events off' -ex 'run' -ex 'thread apply all bt' -ex 'quit' --tty=/dev/stdout --args $rootdir/test/app/nvme_fused/nvme_fused -m 0xF0 -r "/var/tmp/nvme_fused" -C $testdir/nvmf_fused.conf -N
$rootdir/test/app/nvme_fused/nvme_fused -m 0xF0 -r "/var/tmp/nvme_fused" -C $testdir/nvmf_fused.conf -N


rm -f $testdir/nvmf_fused.conf
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
timing_exit nvmf_fused_test
