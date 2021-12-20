#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart

if [ "$TEST_TRANSPORT" != tcp ]; then
	echo "Unsupported transport: $TEST_TRANSPORT"
	exit 0
fi

# Enable zero-copy and set in-capsule data size to zero to make sure all requests are using
# zero-copy
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -c 0 --zcopy

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_malloc_create 32 4096 -b malloc0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0 -n 1

# First send IO with verification
$rootdir/test/bdev/bdevperf/bdevperf --json <(gen_nvmf_target_json) \
	-t 10 -q 128 -w verify -o 8192

# Then send IO in the background while pausing/resuming the subsystem
$rootdir/test/bdev/bdevperf/bdevperf --json <(gen_nvmf_target_json) \
	-t 5 -q 128 -w randrw -M 50 -o 8192 &
perfpid=$!

while kill -0 $perfpid; do
	# Add the same namespace again.  It'll fail, but will also pause/resume the subsystem and
	# the namespace forcing the IO requests to be queued/resubmitted.
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0 -n 1 &> /dev/null || :
done

wait $perfpid

# Verify that aborting requests serviced through zero-copy works too
$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 1
$rpc_py bdev_delay_create -b malloc0 -d delay0 -r 1000000 -t 1000000 -w 1000000 -n 1000000
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 delay0 -n 1

$SPDK_EXAMPLE_DIR/abort -c 0x1 -t 5 -q 64 -w randrw -M 50 -l warning \
	-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT ns:1"

trap - SIGINT SIGTERM EXIT
nvmftestfini
