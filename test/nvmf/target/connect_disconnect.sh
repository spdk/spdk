#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

# connect disconnect is geared towards ensuring that we are properly freeing resources after disconnecting qpairs.
nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -c 0

bdev="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

if [ $RUN_NIGHTLY -eq 1 ]; then
	num_iterations=200
	IO_QUEUES="-i 8"
else
	num_iterations=10
fi

set +x
for i in $(seq 1 $num_iterations); do
	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" $IO_QUEUES
	waitforserial "$NVMF_SERIAL"
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	waitforserial_disconnect "$NVMF_SERIAL"
done
set -x

trap - SIGINT SIGTERM EXIT

nvmftestfini
