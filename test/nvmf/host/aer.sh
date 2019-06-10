#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

timing_enter aer
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

$rpc_py construct_malloc_bdev 64 512 --name Malloc0
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 2
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rpc_py get_nvmf_subsystems

AER_TOUCH_FILE=/tmp/aer_touch_file
rm -f $AER_TOUCH_FILE

# Namespace Attribute Notice Tests
$rootdir/test/nvme/aer/aer -r "\
        trtype:$TEST_TRANSPORT \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2016-06.io.spdk:cnode1" -n 2 -t $AER_TOUCH_FILE &
aerpid=$!

# Waiting for aer start to work
waitforfile $AER_TOUCH_FILE

# Add a new namespace
$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1 -n 2
$rpc_py get_nvmf_subsystems

wait $aerpid

$rpc_py delete_malloc_bdev Malloc0
$rpc_py delete_malloc_bdev Malloc1
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit aer
