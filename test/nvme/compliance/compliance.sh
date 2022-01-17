#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

export TEST_TRANSPORT=VFIOUSER

rm -rf /var/run/vfio-user

# Start the target
"${NVMF_APP[@]}" -m 0x7 &
nvmfpid=$!
echo "Process pid: $nvmfpid"

trap 'killprocess $nvmfpid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $nvmfpid

sleep 1

nqn=nqn.2021-09.io.spdk:cnode0
traddr=/var/run/vfio-user

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT

mkdir -p $traddr

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b malloc0
$rpc_py nvmf_create_subsystem $nqn -a -s spdk -m 32
$rpc_py nvmf_subsystem_add_ns $nqn malloc0
$rpc_py nvmf_subsystem_add_listener $nqn -t $TEST_TRANSPORT -a $traddr -s 0

$testdir/nvme_compliance -g -r "trtype:$TEST_TRANSPORT traddr:$traddr subnqn:$nqn"

killprocess $nvmfpid

rm -rf /var/run/vfio-user

trap - SIGINT SIGTERM EXIT
