#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
rpc_py=$rootdir/scripts/rpc.py

source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

null_bdev_size=1024
null_block_size=512
null_bdev=null0
nvme_bdev=nvme0

if [ "$TEST_TRANSPORT" != "tcp" ]; then
	echo "This test can only be executed with TCP for now"
	exit 0
fi

nvmftestinit
nvmfappstart -m 0x1

# First create a null bdev and expose it over NVMeoF
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py bdev_null_create $null_bdev $null_bdev_size $null_block_size
$rpc_py bdev_wait_for_examine
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 $null_bdev
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Then attach NVMe bdev by connecting back to itself, with the target app running on a single core.
# This verifies that the initialization is completely asynchronous, as each blocking call would
# stall the application.
$rpc_py bdev_nvme_attach_controller -b $nvme_bdev -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-f ipv4 -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode0

# Make sure the bdev was created successfully
$rpc_py bdev_get_bdevs -b ${nvme_bdev}n1

# Make sure the reset is also asynchronous
$rpc_py bdev_nvme_reset_controller $nvme_bdev

# And that the bdev is still available after a reset
$rpc_py bdev_get_bdevs -b ${nvme_bdev}n1

# Finally, detach the controller to verify the detach path
$rpc_py bdev_nvme_detach_controller $nvme_bdev

trap - SIGINT SIGTERM EXIT
nvmftestfini
