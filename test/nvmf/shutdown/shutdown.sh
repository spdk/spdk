#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter shutdown
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
pid=$!

trap "killprocess $pid; nvmfcleanup; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
timing_exit start_nvmf_tgt

num_subsystems=10
# SoftRoce does not have enough queues available for
# this test. Detect if we're using software RDMA.
# If so, only use one subsystem.
if check_ip_is_soft_roce "$NVMF_FIRST_TARGET_IP"; then
	num_subsystems=1
fi

# Create subsystems
for i in `seq 1 $num_subsystems`
do
	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i} "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK${i} -n "$bdevs"
done

modprobe -v nvme-rdma
modprobe -v nvme-fabrics

# Connect kernel host to subsystems
for i in `seq 1 $num_subsystems`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done

# Kill nvmf tgt without removing any subsystem to check whether it can shutdown correctly
rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

killprocess $pid

nvmfcleanup
timing_exit shutdown
