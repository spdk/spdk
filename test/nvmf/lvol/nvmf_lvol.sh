#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512
LVOL_BDEV_SIZE=10
SUBSYS_NR=10

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/nvmf_fio.py"

function disconnect_nvmf()
{
	for i in `seq 1 $SUBSYS_NR`; do
		nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	done
}

set -e

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
$NVMF_APP -c $testdir/../nvmf.conf &
pid=$!

trap "disconnect_nvmf; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

lvol_stores=()
lvol_bdevs=()

# Create malloc backends and creat lvol store on each
for i in `seq 1 $SUBSYS_NR`; do
	bdev="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	ls_guid="$($rpc_py construct_lvol_store $bdev lvs_$i -c 1048576)"
	lvol_stores+=("$ls_guid")

	# 1 NVMe-OF subsystem per malloc bdev / lvol store / 10 lvol bdevs
	ns_bdevs=""

	# Create lvol bdevs on each lvol store
	for j in `seq 1 10`; do
		lb_name="$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$j $LVOL_BDEV_SIZE)"
		lvol_bdevs+=("$lb_name")
		ns_bdevs+="$lb_name "
	done
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK$i -n "$ns_bdevs"
done

for i in `seq 1 $SUBSYS_NR`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done

dev_list=$(get_devices_list nvmf)
$fio_py $dev_list 262144 64 randwrite 10 verify

sync
disconnect_nvmf

for i in `seq 1 $SUBSYS_NR`; do
    $rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i
done

rm -f ./local-job*

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
timing_exit lvol_integrity
