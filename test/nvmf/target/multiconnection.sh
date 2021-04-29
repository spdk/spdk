#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

NVMF_SUBSYS=11

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xF

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	NVMF_SUBSYS=1
fi

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

for i in $(seq 1 $NVMF_SUBSYS); do
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Malloc$i
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

for i in $(seq 1 $NVMF_SUBSYS); do
	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	waitforserial SPDK$i
done

$rootdir/scripts/fio-wrapper -p nvmf -i 262144 -d 64 -t read -r 10
$rootdir/scripts/fio-wrapper -p nvmf -i 262144 -d 64 -t randwrite -r 10

sync
for i in $(seq 1 $NVMF_SUBSYS); do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode${i}
done

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmftestfini
