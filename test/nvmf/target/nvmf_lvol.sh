#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
LVOL_BDEV_INIT_SIZE=20
LVOL_BDEV_FINAL_SIZE=30

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./nvmf_lvol.sh iso
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
	SUBSYS_NR=1
fi

timing_enter lvol_integrity
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0x7 &
pid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $pid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

# Construct a RAID volume for the logical volume store
base_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
base_bdevs+=$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)
$rpc_py construct_raid_bdev -n raid0 -z 64 -r 0 -b "$base_bdevs"

# Create the logical volume store on the RAID volume
lvs=$($rpc_py construct_lvol_store raid0 lvs)

# Create a logical volume on the logical volume store
lvol=$($rpc_py construct_lvol_bdev -u $lvs lvol $LVOL_BDEV_INIT_SIZE)

# Create an NVMe-oF subsystem and add the logical volume as a namespace
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 $lvol
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Start random writes in the background
$rootdir/examples/nvme/perf/perf -r "trtype:RDMA adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -o 4096 -q 128 -s 512 -w randwrite -t 10 -c 0x18 &
perf_pid=$!

sleep 1

# Perform some operations on the logical volume
snapshot=$($rpc_py snapshot_lvol_bdev $lvol "MY_SNAPSHOT")
$rpc_py resize_lvol_bdev $lvol $LVOL_BDEV_FINAL_SIZE
clone=$($rpc_py clone_lvol_bdev $snapshot "MY_CLONE")
$rpc_py inflate_lvol_bdev $clone

# Wait for I/O to complete
wait $perf_pid

# Clean up
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode0
$rpc_py destroy_lvol_bdev $lvol
$rpc_py destroy_lvol_store -u $lvs

rm -f ./local-job*

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
nvmftestfini $1
timing_exit lvol_integrity
