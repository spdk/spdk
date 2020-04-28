#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NULL_BDEV_SIZE=102400
NULL_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

if ! hash nvme; then
	echo "nvme command not found; skipping create transport test"
	exit 0
fi

nvmftestinit
nvmfappstart -m 0xF

# Use nvmf_create_transport call to create transport
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

null_bdevs="$($rpc_py bdev_null_create Null0 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE) "
null_bdevs+="$($rpc_py bdev_null_create Null1 $NULL_BDEV_SIZE $NULL_BLOCK_SIZE)"

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
for null_bdev in $null_bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $null_bdev
done
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme discover -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "Perform nvmf subsystem discovery via RPC"
$rpc_py nvmf_get_subsystems

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

for null_bdev in $null_bdevs; do
	$rpc_py bdev_null_delete $null_bdev
done

check_bdevs=$($rpc_py bdev_get_bdevs | jq -r '.[].name')
if [ -n "$check_bdevs" ]; then
	echo $check_bdevs
	exit 1
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
