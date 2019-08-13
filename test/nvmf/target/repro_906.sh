#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

timing_enter lvol_repro
nvmftestinit
nvmfappstart "-m 0xFF"

$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config

$rpc_py construct_lvol_store Nvme0n1 Lvs0
$rpc_py construct_lvol_bdev -t Lvol0 400000 -l Lvs0
$rpc_py construct_lvol_bdev -t Lvol1 400000 -l Lvs0
$rpc_py get_lvol_stores
$rpc_py get_bdevs

# SoftRoce does not have enough queues available for
# multiconnection tests. Detect if we're using software RDMA.
# If so - lower the number of subsystems for test.
if check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	echo "Using software RDMA, lowering number of NVMeOF subsystems."
	SUBSYS_NR=1
fi

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Create an NVMe-oF subsystem and add the logical volume as a namespace
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Lvs0/Lvol0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Lvs0/Lvol1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode0" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforblk "nvme0n1"
waitforblk "nvme0n2"

fio_bdev --bs=4K \
--numjobs=16 \
--iodepth=16 \
--ioengine=libaio \
--size=100% \
--direct=1 \
--time_based \
--runtime=3M \
--filename=/dev/nvme0n1 \
--name=write \
--rw=randwrite
sleep 3

fio_bdev --bs=4K \
--numjobs=16 \
--iodepth=16 \
--ioengine=libaio \
--size=100% \
--direct=1 \
--time_based \
--runtime=3M \
--filename=/dev/nvme0n2 \
--name=write \
--rw=randwrite
sleep 3

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

# Clean up
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode0
$rpc_py destroy_lvol_bdev Lvs0/Lvol0
$rpc_py destroy_lvol_bdev Lvs0/Lvol1
$rpc_py destroy_lvol_store -u Lvs0


trap - SIGINT SIGTERM EXIT

nvmftestfini
timing_exit lvol_repro
