#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
	echo DEPENDENCY_DIR not defined!
	exit 1
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
devs=()

nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL -d SPDK_Controller1 -i 291
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme discover -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
devs=($(get_nvme_devs)) nvme_num_before_connection=${#devs[@]}
nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforserial $NVMF_SERIAL 2
if [[ -z $(get_nvme_devs) ]]; then
	echo "Could not find any nvme devices to work with, aborting the test" >&2
	exit 1
fi

for ctrl in "${nvmes[@]}"; do
	nvme id-ctrl $ctrl
	nvme smart-log $ctrl
	nvme_model=$(nvme id-ctrl $ctrl | grep -w mn | sed 's/^.*: //' | sed 's/ *$//')
	if [ "$nvme_model" != "SPDK_Controller1" ]; then
		echo "Wrong model number for controller" $nvme_model
		exit 1
	fi
	nvme_cntlid=$(nvme id-ctrl $ctrl | grep -w cntlid | sed 's/^.*: //' | sed 's/ *$//')
	if [ "$nvme_cntlid" != "0x123" ]; then
		echo "Wrong controller ID for controller" $nvme_model
		exit 1
	fi
done

for ns in "${nvmes[@]}"; do
	nvme id-ns $ns
done

devs=($(get_nvme_devs)) nvme_num=${#devs[@]}
nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
if ((nvme_num <= nvme_num_before_connection)); then
	echo "nvme-cli connect target devices failed"
	exit 1
fi

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1
trap - SIGINT SIGTERM EXIT

nvmftestfini
