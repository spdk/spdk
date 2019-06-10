#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/scripts/common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 0
fi

timing_enter fio
timing_enter start_nvmf_tgt

$NVMF_APP -m 0xF &
nvmfpid=$!

trap "process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
timing_exit start_nvmf_tgt

$rpc_py construct_malloc_bdev 64 512 -b Malloc1
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

# Test fio_plugin as host with malloc backend
fio_nvme $PLUGIN_DIR/example_config.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 \
traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1"

# second test mocking multiple SGL elements
fio_nvme $PLUGIN_DIR/mock_sgl_config.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 \
traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1"
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

if [ $RUN_NIGHTLY -eq 1 ]; then
	# Test fio_plugin as host with nvme lvol backend
	bdfs=$(iter_pci_class_code 01 08 02)
	$rpc_py construct_nvme_bdev -b Nvme0 -t PCIe -a $(echo $bdfs | awk '{ print $1 }') -i $NVMF_FIRST_TARGET_IP
	ls_guid=$($rpc_py construct_lvol_store -c 1073741824 Nvme0n1 lvs_0)
	get_lvs_free_mb $ls_guid
	$rpc_py construct_lvol_bdev -l lvs_0 lbd_0 $free_mb
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode2 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode2 lvs_0/lbd_0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode2 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	fio_nvme $PLUGIN_DIR/example_config.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1"
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode2

	# Test fio_plugin as host with nvme lvol nested backend
	ls_nested_guid=$($rpc_py construct_lvol_store --clear-method none lvs_0/lbd_0 lvs_n_0)
	get_lvs_free_mb $ls_nested_guid
	$rpc_py construct_lvol_bdev -l lvs_n_0 lbd_nest_0 $free_mb
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode3 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode3 lvs_n_0/lbd_nest_0
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode3 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	fio_nvme $PLUGIN_DIR/example_config.fio --filename="trtype=$TEST_TRANSPORT adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP trsvcid=$NVMF_PORT ns=1"
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode3

	sync
	# Delete lvol_bdev and destroy lvol_store.
	$rpc_py destroy_lvol_bdev lvs_n_0/lbd_nest_0
	$rpc_py destroy_lvol_store -l lvs_n_0
	$rpc_py destroy_lvol_bdev lvs_0/lbd_0
	$rpc_py destroy_lvol_store -l lvs_0
	$rpc_py delete_nvme_controller Nvme0
fi

trap - SIGINT SIGTERM EXIT

rm -f ./local-test-0-verify.state
nvmftestfini
timing_exit fio
