#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./nmic.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
NVMF_SECOND_TARGET_IP=$(echo "$RDMA_IP_LIST" | sed -n 2p)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter nmic
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -m 0xF --wait-for-rpc &
pid=$!

trap "killprocess $pid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_nvmf_target_options -u 8192 -p 4
$rpc_py start_subsystem_init
timing_exit start_nvmf_tgt

# Create subsystems
bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK1 -n "$bdevs"

set +e
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode2 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK2 -n "$bdevs"
nmic_status=$?

if [ $nmic_status -eq 0 ]; then
	echo " constructing nvmf subsystem passed - failure expected."
	killprocess $pid
	exit 1
else
	echo " constructing nvmf subsystem failed - expected result."
fi
set -e

if [ ! -z $NVMF_SECOND_TARGET_IP ]; then
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT,trtype:RDMA traddr:$NVMF_SECOND_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK1 -n "$bdevs"

	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_SECOND_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme0n1"

	$testdir/../fio/nvmf_fio.py 4096 1 write 1 verify

fi

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $pid

nvmftestfini $1
timing_exit nmic
