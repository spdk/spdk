#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fuzz_test

$NVMF_APP -m 0xF >$output_dir/nvmf_fuzz_tgt_output.txt 2>&1 &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4

bdev=$($rpc_py construct_malloc_bdev 64 512)

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420

echo "[Nvme]" > $testdir/nvmf_fuzz.conf
echo "  TransportID \"trtype:RDMA adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420\" Nvme0" >> $testdir/nvmf_fuzz.conf

$rootdir/test/app/fuzz/nvme_fuzz -t 5 -c $testdir/nvmf_fuzz.conf -n

rm -f $testdir/nvmf_fuzz.conf
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid

nvmftestfini $1
timing_exit fuzz_test
