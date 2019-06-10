#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

timing_enter nmic
nvmftestinit
nvmfappstart "-m 0xF"

NVMF_SECOND_TARGET_IP=$(echo "$RDMA_IP_LIST" | sed -n 2p)

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

# Create subsystems
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK1
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"

echo "test case1: single bdev can't be used in multiple subsystems"
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode2 -a -s SPDK2
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode2 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
nmic_status=0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode2 Malloc0 || nmic_status=$?

if [ $nmic_status -eq 0 ]; then
	echo " Adding namespace passed - failure expected."
	nvmfcleanup
	nvmftestfini
	exit 1
else
	echo " Adding namespace failed - expected result."
fi

echo "test case2: host connect to nvmf target in multiple paths"
if [ ! -z $NVMF_SECOND_TARGET_IP ]; then
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_PORT

	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme0n1"

	$rootdir/scripts/fio.py -p nvmf -i 4096 -d 1 -t write -r 1 -v
fi

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit nmic
