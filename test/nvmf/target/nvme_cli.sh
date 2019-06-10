#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ -z "${DEPENDENCY_DIR}" ]; then
        echo DEPENDENCY_DIR not defined!
        exit 1
fi

spdk_nvme_cli="${DEPENDENCY_DIR}/nvme-cli"

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter nvme_cli
nvmftestinit
nvmfappstart "-m 0xF"

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1

$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforblk "nvme0n1"
waitforblk "nvme0n2"

nvme list

for ctrl in /dev/nvme?; do
	nvme id-ctrl $ctrl
	nvme smart-log $ctrl
	nvme_model=$(nvme id-ctrl $ctrl | grep -w mn | sed 's/^.*: //' | sed 's/ *$//')
	if [ "$nvme_model" != "SPDK_Controller1" ]; then
		echo "Wrong model number for controller" $nvme_model
		exit 1
	fi
done

for ns in /dev/nvme?n*; do
	nvme id-ns $ns
done

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"

if [ -d  $spdk_nvme_cli ]; then
	# Test spdk/nvme-cli NVMe-oF commands: discover, connect and disconnect
	cd $spdk_nvme_cli
	sed -i 's/shm_id=.*/shm_id=-1/g' spdk.conf
	./nvme discover -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
	nvme_num_before_connection=$(nvme list |grep "/dev/nvme*"|awk '{print $1}'|wc -l)
	./nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	sleep 1
	nvme_num=$(nvme list |grep "/dev/nvme*"|awk '{print $1}'|wc -l)
	./nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	if [ $nvme_num -le $nvme_num_before_connection ]; then
		echo "spdk/nvme-cli connect target devices failed"
		exit 1
	fi
fi

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
report_test_completion "nvmf_spdk_nvme_cli"
timing_exit nvme_cli
