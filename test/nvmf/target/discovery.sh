#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NULL_BDEV_SIZE=102400
NULL_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

if ! hash nvme; then
	echo "nvme command not found; skipping discovery test"
	exit 0
fi

timing_enter discovery
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

null_bdevs="$($rpc_py construct_null_bdev Null0 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE) "
null_bdevs+="$($rpc_py construct_null_bdev Null1 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE)"

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for null_bdev in $null_bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $null_bdev
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme discover -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "Perform nvmf subsystem discovery via RPC"
$rpc_py get_nvmf_subsystems

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

for null_bdev in $null_bdevs; do
	$rpc_py delete_null_bdev $null_bdev
done

check_bdevs=$($rpc_py get_bdevs | jq -r '.[].name')
if [ -n "$check_bdevs" ]; then
	echo $check_bdevs
	exit 1
fi

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit discovery
