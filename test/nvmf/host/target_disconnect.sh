#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter target_disconnect

function target_init()
{
	nvmfappstart "-m 0xF"

	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
}

nvmftestinit
target_init

# If fio doesn't shut down, this test will time out.
cp $PLUGIN_DIR/example_config.fio $PLUGIN_DIR/example_config_extended.fio
sed -i "s/runtime=2/runtime=600/" $PLUGIN_DIR/example_config_extended.fio
fio_nvme $PLUGIN_DIR/example_config_extended.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1" || true &

sleep 2
kill -9 $nvmfpid

wait

target_init

# Detect if perf exits with an error code.
$rootdir/examples/nvme/perf/perf -q 32 -o 4096 -w randrw -M 50 -t 15 -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" &
perfpid=$!

sleep 2
kill -9 $nvmfpid
# sleep for 4 seconds so we can try at least one failed reconnect to make sure nothing breaks.
sleep 4

target_init
wait $perfpid

sync

trap - SIGINT SIGTERM EXIT
rm -f $PLUGIN_DIR/example_config_extended.fio || true
nvmftestfini
timing_exit target_disconnect
