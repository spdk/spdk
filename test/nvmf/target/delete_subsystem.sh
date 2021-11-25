#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0x3

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_null_create NULL1 1000 512
# Subsystem destruction process waits for all controllers to be destroyed which in turn wait
# for all qpairs to be deleted. A qpair can only be deleted when all outstanding requests are completed
# bdev_delay is used in this test to make a situation when qpair has outstanding requests when disconnect
# starts. It allows to trigger async qpair/controller/subsystem destruction path
$rpc_py bdev_delay_create -b NULL1 -d Delay0 -r 1000000 -t 1000000 -w 1000000 -n 1000000
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0

$SPDK_EXAMPLE_DIR/perf -c 0xC -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-t 5 -q 128 -w randrw -M 70 -o 512 -P 4 &
perf_pid=$!

sleep 2

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

delay=0
while kill -0 $perf_pid; do
	sleep 0.5
	#wait 15 seconds max
	if ((delay++ > 30)); then
		echo "perf is still running, failing the test"
		false
	fi
done

#check that traffic goes when a new subsystem is created
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0

$SPDK_EXAMPLE_DIR/perf -c 0xC -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-t 3 -q 128 -w randrw -M 70 -o 512 -P 4 &
perf_pid=$!

delay=0
while kill -0 $perf_pid; do
	sleep 0.5
	#wait 10 seconds max
	if ((delay++ > 20)); then
		echo "perf didn't finish on time"
		false
	fi
done

trap - SIGINT SIGTERM EXIT

nvmftestfini
