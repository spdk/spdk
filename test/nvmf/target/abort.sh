#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xE

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -a 256

# Construct a delay bdev on a malloc bdev which has constant 10ms delay for all read or write I/Os
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py bdev_delay_create -b Malloc0 -d Delay0 -r 1000000 -t 1000000 -w 1000000 -n 1000000

# Create an NVMe-oF subsystem and add the delay bdev as a namespace
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Delay0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Run abort application
$SPDK_EXAMPLE_DIR/abort -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-c 0x1 -t 1 -l warning -q 128

# Clean up
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode0

trap - SIGINT SIGTERM EXIT

nvmftestfini
