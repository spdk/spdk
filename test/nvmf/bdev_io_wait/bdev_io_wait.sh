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
# e.g. sudo ./bdev_io_wait.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter bdev_io_wait
timing_enter start_nvmf_tgt

$NVMF_APP -m 0xF --wait-for-rpc &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
# Minimal number of bdev io pool (5) and cache (1)
$rpc_py set_bdev_options -p 5 -c 1
$rpc_py start_subsystem_init
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
$rpc_py nvmf_subsystem_add_listener  nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420

echo "[Nvme]" > $testdir/bdevperf.conf
echo "  TransportID \"trtype:RDMA adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420\" Nvme0" >> $testdir/bdevperf.conf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w write -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w read -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w flush -t 1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w unmap -t 1
sync
rm -rf $testdir/bdevperf.conf
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
nvmftestfini $1
timing_exit bdev_io_wait
