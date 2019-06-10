#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

NVMF_SUBSYS=11

rpc_py="$rootdir/scripts/rpc.py"

timing_enter multiconnection
nvmftestinit
nvmfappstart "-m 0xF"

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	NVMF_SUBSYS=1
fi

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

for i in $(seq 1 $NVMF_SUBSYS)
do
	$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Malloc$i
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

for i in $(seq 1 $NVMF_SUBSYS); do
	k=$[$i-1]
	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme${k}n1"
done

$rootdir/scripts/fio.py -p nvmf -i 262144 -d 64 -t read -r 10
$rootdir/scripts/fio.py -p nvmf -i 262144 -d 64 -t randwrite -r 10

sync
for i in $(seq 1 $NVMF_SUBSYS); do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i}
done

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit multiconnection
