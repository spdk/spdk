#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/scripts/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

timing_enter nvme_identify

bdf=$(get_first_nvme_bdf)
if [ -z "${bdf}" ]; then
	echo "No NVMe drive found but test requires it. Failing the test."
	exit 1
fi

# Expected values
nvme_serial_number=$($SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:${bdf}" -i 0 | grep "Serial Number:" | awk '{print $3}')
nvme_model_number=$($SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:${bdf}" -i 0 | grep "Model Number:" | awk '{print $3}')

timing_exit nvme_identify

timing_enter start_nvmf_tgt

"${NVMF_APP[@]}" -m 0xF --wait-for-rpc &
nvmfpid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py -v nvmf_set_config --passthru-identify-ctrlr
$rpc_py -v framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
timing_exit start_nvmf_tgt

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 1
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme0n1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rpc_py nvmf_get_subsystems

# Discovered values
nvmf_serial_number=$($SPDK_EXAMPLE_DIR/identify -r "\
        trtype:$TEST_TRANSPORT \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2016-06.io.spdk:cnode1" | grep "Serial Number:" | awk '{print $3}')

nvmf_model_number=$($SPDK_EXAMPLE_DIR/identify -r "\
        trtype:$TEST_TRANSPORT \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2016-06.io.spdk:cnode1" | grep "Model Number:" | awk '{print $3}')

if [ ${nvme_serial_number} != ${nvmf_serial_number} ]; then
	echo "Serial number doesn't match"
	exit 1
fi

if [ ${nvme_model_number} != ${nvmf_model_number} ]; then
	echo "Model number doesn't match"
	exit 1
fi

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
