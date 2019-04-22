#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./fio.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter srq_overwhelm
timing_enter start_nvmf_tgt

$NVMF_APP -m 0xF &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
# create the rdma transport with an intentionally small SRQ depth
$rpc_py nvmf_create_transport -t RDMA -u 8192 -s 1024
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma
declare -a malloc_bdevs
malloc_bdevs[0]="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
malloc_bdevs[1]+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
malloc_bdevs[2]+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
malloc_bdevs[3]+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
malloc_bdevs[4]+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
malloc_bdevs[5]+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

for i in $(seq 0 5); do
	let j=$i+1
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$j -a -s SPDK00000000000001
	echo ${malloc_bdevs[i]}
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$j "${malloc_bdevs[i]}"
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$j -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${j}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	waitforblk "nvme${i}n1"
done

$rootdir/scripts/fio.py nvmf 1048576 128 read 10 13

sync

for i in $(seq 1 6); do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}"
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i
done

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
nvmftestfini $1
timing_exit srq_overwhelm
