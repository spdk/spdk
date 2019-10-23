#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

function disconnect_init()
{
	nvmfappstart "-m 0xF0"

	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $1 -s $NVMF_PORT
}

timing_enter target_disconnect

nvmftestinit

disconnect_init $NVMF_FIRST_TARGET_IP

# If perf doesn't shut down, this test will time out.
$rootdir/examples/nvme/reconnect/reconnect -q 32 -o 4096 -w randrw -M 50 -t 10 -c 0xF \
-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" &
reconnectpid=$!

sleep 2
kill -9 $nvmfpid

sleep 2
disconnect_init $NVMF_FIRST_TARGET_IP

wait $reconnectpid
sync

if [ -n "$NVMF_SECOND_TARGET_IP" ]; then
	timing_enter failover_test

	$rootdir/examples/nvme/reconnect/reconnect -q 32 -o 4096 -w randrw -M 50 -t 10 -c 0xF \
	-r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT alt_traddr:$NVMF_SECOND_TARGET_IP" &
	reconnectpid=$!

	sleep 2
	kill -9 $nvmfpid

	sleep 2
	disconnect_init $NVMF_SECOND_TARGET_IP

	wait $reconnectpid
	sync

	timing_exit failover_test
fi

trap - SIGINT SIGTERM EXIT
rm -f $PLUGIN_DIR/example_config_extended.fio || true
nvmftestfini
timing_exit target_disconnect
