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

nvmftestinit
nvmfappstart "-m 0xF"

$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0

$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# If fio doesn't shut down, this test will time out.
cp $PLUGIN_DIR/example_config.fio $PLUGIN_DIR/example_config_extended.fio
sed -i "s/runtime=2/runtime=600/" $PLUGIN_DIR/example_config_extended.fio
fio_nvme $PLUGIN_DIR/example_config_extended.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1" || true &
sleep 10
sudo kill -9 $nvmfpid
wait

sync

trap - SIGINT SIGTERM EXIT

rm -f $PLUGIN_DIR/example_config_extended.fio || true
nvmftestfini
timing_exit target_disconnect
