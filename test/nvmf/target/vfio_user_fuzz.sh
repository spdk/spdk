#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
nqn=nqn.2021-09.io.spdk:cnode0
traddr=/var/run/vfio-user

export TEST_TRANSPORT=VFIOUSER

rm -rf $traddr

# Start the target
"${NVMF_APP[@]}" -m 0x1 > $output_dir/vfio_user_fuzz_tgt_output.txt 2>&1 &
nvmfpid=$!
echo "Process pid: $nvmfpid"

trap 'killprocess $nvmfpid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $nvmfpid

sleep 1

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT

mkdir -p $traddr

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b malloc0
$rpc_py nvmf_create_subsystem $nqn -a -s spdk
$rpc_py nvmf_subsystem_add_ns $nqn malloc0
$rpc_py nvmf_subsystem_add_listener $nqn -t $TEST_TRANSPORT -a $traddr -s 0

trid="trtype:$TEST_TRANSPORT subnqn:$nqn traddr:$traddr"

$rootdir/test/app/fuzz/nvme_fuzz/nvme_fuzz -m 0x2 -r "/var/tmp/vfio_user_fuzz" -t 30 -S 123456 -F "$trid" -N -a 2> $output_dir/vfio_user_fuzz_log.txt
$rpc_py nvmf_delete_subsystem $nqn

killprocess $nvmfpid

rm -rf $traddr

trap - SIGINT SIGTERM EXIT
