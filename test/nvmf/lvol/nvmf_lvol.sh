#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
LVOL_BDEV_SIZE=10
SUBSYS_NR=2
LVOL_BDEVS_NR=6

rpc_py="$rootdir/scripts/rpc.py"

function disconnect_nvmf()
{
	for i in `seq 1 $SUBSYS_NR`; do
		nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	done
}

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
$NVMF_APP -m 0xF &
pid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; disconnect_nvmf; killprocess $pid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

lvol_stores=()
lvol_bdevs=()
# Create the first LVS from a Raid-0 bdev, which is created from two malloc bdevs
# Create remaining LVSs from a malloc bdev, respectively
for i in `seq 1 $SUBSYS_NR`; do
	if [ $i -eq 1 ]; then
		# construct RAID bdev and put its name in $bdev
		malloc_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
		malloc_bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
		$rpc_py construct_raid_bdev -n raid0 -s 64 -r 0 -b "$malloc_bdevs"
		bdev="raid0"
	else
		# construct malloc bdev and put its name in $bdev
		bdev="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	fi
	ls_guid="$($rpc_py construct_lvol_store $bdev lvs_$i -c 524288)"
	lvol_stores+=("$ls_guid")

	# 1 NVMe-OF subsystem per malloc bdev / lvol store / 10 lvol bdevs
	ns_bdevs=""

	# Create lvol bdevs on each lvol store
	for j in `seq 1 $LVOL_BDEVS_NR`; do
		lb_name="$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$j $LVOL_BDEV_SIZE)"
		lvol_bdevs+=("$lb_name")
		ns_bdevs+="$lb_name "
	done

	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
	for bdev in $ns_bdevs; do
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i $bdev
	done
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

for i in `seq 1 $SUBSYS_NR`; do
	k=$[$i-1]
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	for j in `seq 1 $LVOL_BDEVS_NR`; do
		waitforblk "nvme${k}n${j}"
	done
done

$testdir/../fio/nvmf_fio.py 262144 64 randwrite 10 verify

sync
disconnect_nvmf

for i in `seq 1 $SUBSYS_NR`; do
    $rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i
done

rm -f ./local-job*

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
nvmftestfini $1
timing_exit lvol_integrity
