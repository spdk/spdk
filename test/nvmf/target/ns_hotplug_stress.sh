#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0xE

null_size=1000

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_malloc_create 32 512 -b Malloc0
$rpc_py bdev_delay_create -b Malloc0 -d Delay0 -r 1000000 -t 1000000 -w 1000000 -n 1000000
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0
$rpc_py bdev_null_create NULL1 $null_size 512
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 NULL1

# Note: use -Q option to rate limit the error messages that perf will spew due to the
# namespace hotplugs
$SPDK_EXAMPLE_DIR/perf -c 0x1 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-t 30 -q 128 -w randread -o 512 -Q 1000 &
PERF_PID=$!

while kill -0 $PERF_PID; do
	$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 1
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0
	# Also test bdev/namespace resizing here, since it has a similar
	# effect on subsystem/namespace handling in the nvmf target
	null_size=$((null_size + 1))
	$rpc_py bdev_null_resize NULL1 $null_size
done

wait $PERF_PID

trap - SIGINT SIGTERM EXIT

nvmftestfini
