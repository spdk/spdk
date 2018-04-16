#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fio
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000001

for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$bdev"
done

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforblk "nvme0n1"
waitforblk "nvme0n2"

$testdir/nvmf_fio.py 4096 1 write 1 verify
$testdir/nvmf_fio.py 4096 1 randwrite 1 verify
$testdir/nvmf_fio.py 4096 128 write 1 verify
$testdir/nvmf_fio.py 4096 128 randwrite 1 verify

sync

#start hotplug test case
$testdir/nvmf_fio.py 4096 1 read 10 &
fio_pid=$!

sleep 3
set +e

for bdev in $bdevs; do
	$rpc_py delete_bdev "$bdev"
done

wait $fio_pid
fio_status=$?

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

if [ $fio_status -eq 0 ]; then
        echo "nvmf hotplug test: fio successful - expected failure"
        nvmfcleanup
        killprocess $nvmfpid
        exit 1
else
        echo "nvmf hotplug test: fio failed as expected"
fi
set -e

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

rm -f ./local-job0-0-verify.state
rm -f ./local-job1-1-verify.state
rm -f ./local-job2-2-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fio
