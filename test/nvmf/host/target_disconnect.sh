#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

function disconnect_init() {
	nvmfappstart -m 0xF0

	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $1 -s $NVMF_PORT
}

# Test to make sure we don't segfault or access null pointers when we try to connect to
# a discovery controller that doesn't exist yet.
function nvmf_target_disconnect_tc1() {
	set +e
	$SPDK_EXAMPLE_DIR/reconnect -q 32 -o 4096 -w randrw -M 50 -t 10 -c 0xF \
		-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"
	# If the program crashes, the high bit of $? will be set so we will get a value in the hundreds.
	# But if the reconnect code detects errors and exits normally it will return 1.
	if [ $? != 1 ]; then
		set -e
		exit 1
	fi
	set -e
}

function nvmf_target_disconnect_tc2() {
	disconnect_init $NVMF_FIRST_TARGET_IP

	# If perf doesn't shut down, this test will time out.
	$SPDK_EXAMPLE_DIR/reconnect -q 32 -o 4096 -w randrw -M 50 -t 10 -c 0xF \
		-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" &
	reconnectpid=$!

	sleep 2
	kill -9 $nvmfpid

	sleep 2
	disconnect_init $NVMF_FIRST_TARGET_IP

	wait $reconnectpid
	sync
}

function nvmf_target_disconnect_tc3() {
	$SPDK_EXAMPLE_DIR/reconnect -q 32 -o 4096 -w randrw -M 50 -t 10 -c 0xF \
		-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT alt_traddr:$NVMF_SECOND_TARGET_IP" &
	reconnectpid=$!

	sleep 2
	kill -9 $nvmfpid

	sleep 2
	disconnect_init $NVMF_SECOND_TARGET_IP

	wait $reconnectpid
	sync
}

nvmftestinit
run_test "nvmf_target_disconnect_tc1" nvmf_target_disconnect_tc1
run_test "nvmf_target_disconnect_tc2" nvmf_target_disconnect_tc2
if [ -n "$NVMF_SECOND_TARGET_IP" ]; then
	run_test "nvmf_target_disconnect_tc3" nvmf_target_disconnect_tc3
fi

trap - SIGINT SIGTERM EXIT
nvmftestfini
