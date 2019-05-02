#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./rpc.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter rpc
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0xF &
pid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $pid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
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
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
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
waitforblk "nvme0n1"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

# Remove the host and verify that the connect fails
$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
! nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Allow any host and verify that the connect succeeds
$rpc_py nvmf_subsystem_allow_any_host -e nqn.2016-06.io.spdk:cnode1
nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforblk "nvme0n1"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

# do frequent add delete of namespaces with different nsid.
for i in `seq 1 $times`
do
	j=0
	for bdev in $bdevs; do
		let j=j+1
		$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$j -s SPDK00000000000001
		$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$j -t RDMA -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$j $bdev -n 5
		$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode$j
		nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode$j -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	done

	waitforblk "nvme0n1"
	n=$j
	for j in `seq 1 $n`
	do
		nvme disconnect -n nqn.2016-06.io.spdk:cnode$j
	done

	j=0
	for bdev in $bdevs; do
		let j=j+1
		$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode$j 5
	done

	n=$j
	for j in `seq 1 $n`
	do
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j
	done

done

nvmfcleanup
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

# do frequent add delete.
for i in `seq 1 $times`
do
	j=0
	for bdev in $bdevs; do
		let j=j+1
		$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$j -s SPDK00000000000001
		$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$j -t RDMA -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$j $bdev
		$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode$j
	done

	j=0
	for bdev in $bdevs; do
		let j=j+1
		$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode$j $j
	done

	n=$j
	for j in `seq 1 $n`
	do
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j
	done
done

trap - SIGINT SIGTERM EXIT

killprocess $pid
nvmftestfini $1
timing_exit rpc
