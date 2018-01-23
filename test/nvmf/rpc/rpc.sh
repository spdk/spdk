#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter rpc
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
timing_exit start_nvmf_tgt

# set times for subsystem construct/delete
if [ $RUN_NIGHTLY -eq 1 ]; then
	times=50
else
	times=3
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

# Disallow host NQN and make sure connect fails
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 '' '' -s SPDK00000000000001
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
$rpc_py nvmf_subsystem_allow_any_host -d nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

modprobe -v nvme-rdma
trap "killprocess $pid; nvmfcleanup; exit 1" SIGINT SIGTERM EXIT

# This connect should fail - the host NQN is not allowed
! nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Add the host NQN and verify that the connect succeeds
$rpc_py nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

# Remove the host and verify that the connect fails
$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
! nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Allow any host and verify that the connect succeeds
$rpc_py nvmf_subsystem_allow_any_host -e nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:cnode1
nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
nvmfcleanup
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

# do frequent add delete.
for i in `seq 1 $times`
do
	j=0
	for bdev in $bdevs; do
		let j=j+1
		$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j '' '' -s SPDK00000000000001
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$j $bdev
		$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode$j
	done

	n=$j
	for j in `seq 1 $n`
	do
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j
	done
done

trap - SIGINT SIGTERM EXIT

killprocess $pid
timing_exit rpc
