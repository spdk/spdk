#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

timing_enter rpc
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

# set times for subsystem construct/delete
if [ $RUN_NIGHTLY -eq 1 ]; then
	times=50
else
	times=3
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1

# Disallow host NQN and make sure connect fails
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
$rpc_py nvmf_subsystem_allow_any_host -d nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# This connect should fail - the host NQN is not allowed
! nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Add the host NQN and verify that the connect succeeds
$rpc_py nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforblk "nvme0n1"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

# Remove the host and verify that the connect fails
$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
! nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Allow any host and verify that the connect succeeds
$rpc_py nvmf_subsystem_allow_any_host -e nqn.2016-06.io.spdk:cnode1
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforblk "nvme0n1"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

# do frequent add delete of namespaces with different nsid.
for i in $(seq 1 $times)
do
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1 -n 5
	$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode1
	nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme0n1"

	nvme disconnect -n nqn.2016-06.io.spdk:cnode1

	$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 5
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

done

nvmfcleanup

# do frequent add delete.
for i in $(seq 1 $times)
do
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode1

	$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 1

	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
done

trap - SIGINT SIGTERM EXIT

nvmftestfini
timing_exit rpc
