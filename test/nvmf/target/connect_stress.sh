#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xE

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_null_create NULL1 1000 512

$rootdir/test/nvme/connect_stress/connect_stress -c 0x1 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT subnqn:nqn.2016-06.io.spdk:cnode1" -t 10 &
PERF_PID=$!

rpcs=$SPDK_TEST_STORAGE/rpc.txt

rm -f $rpcs

for i in $(seq 1 20); do
	cat <<- EOF >> $rpcs
		nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 NULL1
		nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 1
	EOF
done

while kill -0 $PERF_PID; do
	$rpc_py < $rpcs
done

wait $PERF_PID
rm -f $rpcs

trap - SIGINT SIGTERM EXIT

nvmftestfini
