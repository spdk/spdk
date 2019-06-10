#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter srq_overwhelm
nvmftestinit

if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, Likely not enough memory to run this test. aborting."
	exit 0
fi

nvmfappstart "-m 0xF"

# create the rdma transport with an intentionally small SRQ depth
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192 -s 1024

for i in $(seq 0 5); do
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK00000000000001
	$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Malloc$i
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -i 16
	waitforblk "nvme${i}n1"
done

# by running 6 different FIO jobs, each with 13 subjobs, we end up with 78 fio threads trying to write to
# our target at once. This completely overwhelms the target SRQ, but allows us to verify that rnr_retry is
# working even at very high queue depths because the rdma qpair doesn't fail.
# It is normal to see the initiator timeout and reconnect waiting for completions from an overwhelmmed target,
# but the connection should come up and FIO should complete without errors.
$rootdir/scripts/fio.py -p nvmf -i 1048576 -d 128 -t read -r 10 -n 13

sync

for i in $(seq 0 5); do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}"
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i
done

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit srq_overwhelm
