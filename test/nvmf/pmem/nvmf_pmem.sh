#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

RUNTIME=$1
PMEM_BDEVS=""
SUBSYS_NR=1
PMEM_PER_SUBSYS=8
rpc_py="python $rootdir/scripts/rpc.py"

function disconnect_nvmf()
{
	for i in `seq 1 $SUBSYS_NR`; do
		nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}" || true
	done
}

function clear_pmem_pool()
{
	for pmem in $PMEM_BDEVS; do
		$rpc_py delete_bdev $pmem
	done

	for i in `seq 1 $SUBSYS_NR`; do
		for c in `seq 1 $PMEM_PER_SUBSYS`; do
			$rpc_py delete_pmem_pool /tmp/pool_file${i}_${c}
		done
	done
}

set -e

timing_enter nvmf_pmem

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
pid=$!

trap "disconnect_nvmf; rm -f /tmp/pool_file*; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

timing_enter setup
# Create pmem backends on each subsystem
for i in `seq 1 $SUBSYS_NR`; do
	bdevs=""
	for c in `seq 1 $PMEM_PER_SUBSYS`; do
		$rpc_py create_pmem_pool /tmp/pool_file${i}_${c} 32 512
		bdevs+="$($rpc_py construct_pmem_bdev -n pmem${i}_${c} /tmp/pool_file${i}_${c}) "
	done
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK$i -n "$bdevs"
	PMEM_BDEVS+=$bdevs
done
timing_exit setup

sleep 1

timing_enter nvmf_connect
for i in `seq 1 $SUBSYS_NR`; do
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
done
timing_exit nvmf_connect

timing_enter fio_test
$testdir/../fio/nvmf_fio.py 131072 64 randwrite $RUNTIME verify
timing_exit fio_test

sync
disconnect_nvmf

for i in `seq 1 $SUBSYS_NR`; do
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$i
done

clear_pmem_pool

rm -f ./local-job*

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid
timing_exit nvmf_pmem
