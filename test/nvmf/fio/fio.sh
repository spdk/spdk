#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/nvmf_fio.py"

function cleanup(){
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true
        ps -p $nvmfpid
        retval=$?
        if [ $retval -eq 0 ]; then
                $rpc_py delete_bdev lvs0/lbd0
                $rpc_py destroy_lvol_store -l lvs0
        fi
}

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
$rootdir/scripts/gen_nvme.sh >> $testdir/../nvmf.conf
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

ls_guid="$($rpc_py construct_lvol_store Nvme0n1 lvs0 -c 1048576)"
lb_bdevs="$($rpc_py construct_lvol_bdev -u $ls_guid lbd0 128)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK000000000001 -n "$lb_bdevs"

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

trap "cleanup; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

$fio_py 4096 1 write 1 verify
$fio_py 4096 1 randwrite 1 verify
$fio_py 4096 128 write 1 verify
$fio_py 4096 128 randwrite 1 verify
$fio_py 4096 16 trim 1

if [ $RUN_NIGHTLY -eq 1 ]; then
	$fio_py 1048576 128 randread 30 verify 512M
	$fio_py 4096 16 randread 30 verify 512M
	$fio_py 8192 32 randread 30 verify 512M
	$fio_py 1048576 128 trim 30 verify 512M
	$fio_py 4096 16 trim 30 verify 512M
	$fio_py 12288 128 trim 30 verify 512M
	$fio_py 524288 128 trimwrite 30 verify 512M
	$fio_py 262144 128 trimwrite 30 verify 512M
	$fio_py 131072 128 trimwrite 30 verify 512M
	$fio_py 65536 128 randtrim 30 verify 512M
	$fio_py 32768 128 randtrim 30 verify 512M
	$fio_py 16384 128 randtrim 30 verify 512M
fi
sync

#start hotplug test case
$fio_py 4096 1 read 10 &
fio_pid=$!

sleep 3
set +e

$rpc_py delete_bdev "lvs0/lbd0"
$rpc_py destroy_lvol_store -l "lvs0"
$rpc_py delete_bdev "Nvme0n1"

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

trap - SIGINT SIGTERM EXIT

sed -i '/\[Nvme/,$d' $testdir/../nvmf.conf
nvmfcleanup
killprocess $nvmfpid
timing_exit fio
