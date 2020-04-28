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

nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Use at least 4 subsystems so they spill over to a second discovery log page
for i in $(seq 1 4); do
	$rpc_py bdev_null_create Null$i $NULL_BDEV_SIZE $NULL_BLOCK_SIZE
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$i -a -s SPDK0000000000000$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Null$i
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
done

nvme discover -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

echo "Perform nvmf subsystem discovery via RPC"
$rpc_py nvmf_get_subsystems

for i in $(seq 1 4); do
	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode$i
	$rpc_py bdev_null_delete Null$i
done

check_bdevs=$($rpc_py bdev_get_bdevs | jq -r '.[].name')
if [ -n "$check_bdevs" ]; then
	echo $check_bdevs
	exit 1
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
