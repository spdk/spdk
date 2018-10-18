#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
NVMF_SUBSYS=11

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./multiconnection.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	NVMF_SUBSYS=1
fi

timing_enter multiconnection
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0xF &
pid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $pid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

for i in `seq 1 $NVMF_SUBSYS`
do
	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	for bdev in $bdevs; do
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i $bdev
	done
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

for i in `seq 1 $NVMF_SUBSYS`; do
	k=$[$i-1]
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme${k}n1"
done

$testdir/../fio/nvmf_fio.py 262144 64 read 10
$testdir/../fio/nvmf_fio.py 262144 64 randwrite 10

sync
for i in `seq 1 $NVMF_SUBSYS`; do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode${i}
done

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
nvmftestfini $1
timing_exit multiconnection
