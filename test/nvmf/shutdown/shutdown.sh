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
# If so, only use two subsystem.
if check_ip_is_soft_roce "$NVMF_FIRST_TARGET_IP"; then
	num_subsystems=2
fi

touch $testdir/bdevperf.conf
echo "[Nvme]" > $testdir/bdevperf.conf

# Create subsystems
for i in `seq 1 $num_subsystems`
do
	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	for bdev in $bdevs; do
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i $bdev
	done
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	echo "  TransportID \"trtype:RDMA adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode$i traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT\" Nvme$i" >> $testdir/bdevperf.conf
done

# Test 1: Kill initiator unexpectedly

# Run bdevperf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 64 -o 65536 -w verify -t 20 &
perfpid=$!
sleep 10

# Kill bdevperf half way through
killprocess $perfpid

# Verify the target stays up
sleep 1
kill -0 $pid

# Test 2: Kill the target unexpectedly

# Run bdevperf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 64 -o 65536 -w verify -t 20 &
perfpid=$!

# Expand the trap to clean up bdevperf if something goes wrong
trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $pid; kill -9 $perfpid; nvmfcleanup; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

# Kill the target half way through
sleep 10
killprocess $pid

# Verify bdevperf exits successfully
sleep 1
# TODO: Right now the NVMe-oF initiator will not correctly detect broken connections
# and so it will never shut down. Just kill it.
kill -9 $perfpid

rm -f ./local-job0-0-verify.state
rm -rf $testdir/bdevperf.conf
trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini $1
timing_exit shutdown
