#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./shutdown.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter shutdown
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0xF &
pid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $pid; nvmfcleanup; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

num_subsystems=10
# SoftRoce does not have enough queues available for
# this test. Detect if we're using software RDMA.
# If so, only use four subsystems.
if check_ip_is_soft_roce "$NVMF_FIRST_TARGET_IP"; then
	num_subsystems=4
fi

# Create subsystems
for i in `seq 1 $num_subsystems`
do
	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	for bdev in $bdevs; do
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i $bdev
	done
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

modprobe -v nvme-rdma
modprobe -v nvme-fabrics

# Repeatedly connect and disconnect
for ((x=0; x<5;x++)); do
	# Connect kernel host to subsystems
	for i in `seq 1 $num_subsystems`; do
		nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	done
	# Disconnect the subsystems in reverse order
	for i in `seq $num_subsystems -1 1`; do
		nvme disconnect -n nqn.2016-06.io.spdk:cnode${i}
	done
done

# Start a series of connects right before disconnecting
for i in `seq 1 $num_subsystems`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done

waitforblk "nvme0n1"

# Kill nvmf tgt without removing any subsystem to check whether it can shutdown correctly
rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

killprocess $pid

nvmfcleanup
nvmftestfini $1
timing_exit shutdown
