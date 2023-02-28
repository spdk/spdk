#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
nvmeappdir=$(readlink -f $rootdir/test/nvme)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NUM_DEVICES=2

rpc_py="$rootdir/scripts/rpc.py"

export TEST_TRANSPORT=VFIOUSER

function aer_vfio_user() {

	local traddr=$1
	local subnqn=$2
	local malloc_num=Malloc$(($3 + NUM_DEVICES))
	$rpc_py nvmf_get_subsystems

	AER_TOUCH_FILE=/tmp/aer_touch_file

	# Namespace Attribute Notice Tests
	$rootdir/test/nvme/aer/aer -r "\
		trtype:$TEST_TRANSPORT \
		traddr:$traddr \
		subnqn:$subnqn" -n $NUM_DEVICES -g -t $AER_TOUCH_FILE &
	aerpid=$!

	# Waiting for aer start to work
	waitforfile $AER_TOUCH_FILE
	rm -f $AER_TOUCH_FILE
	# Add a new namespace
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE --name $malloc_num
	$rpc_py nvmf_subsystem_add_ns $subnqn $malloc_num -n $NUM_DEVICES
	$rpc_py nvmf_get_subsystems

	wait $aerpid
}

rm -rf /var/run/vfio-user

# Start the target
"${NVMF_APP[@]}" -m '[0,1,2,3]' &
nvmfpid=$!
echo "Process pid: $nvmfpid"

trap 'killprocess $nvmfpid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $nvmfpid

sleep 1

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT

mkdir -p /var/run/vfio-user

for i in $(seq 1 $NUM_DEVICES); do
	mkdir -p /var/run/vfio-user/domain/vfio-user$i/$i

	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
	$rpc_py nvmf_create_subsystem nqn.2019-07.io.spdk:cnode$i -a -s SPDK$i
	$rpc_py nvmf_subsystem_add_ns nqn.2019-07.io.spdk:cnode$i Malloc$i
	$rpc_py nvmf_subsystem_add_listener nqn.2019-07.io.spdk:cnode$i -t $TEST_TRANSPORT -a "/var/run/vfio-user/domain/vfio-user$i/$i" -s 0
done

for i in $(seq 1 $NUM_DEVICES); do
	test_traddr=/var/run/vfio-user/domain/vfio-user$i/$i
	test_subnqn=nqn.2019-07.io.spdk:cnode$i
	$SPDK_EXAMPLE_DIR/identify -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn" -g -L nvme -L nvme_vfio -L vfio_pci
	$SPDK_EXAMPLE_DIR/perf -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn" -s 256 -g -q 128 -o 4096 -w read -t 5 -c 0x2
	$SPDK_EXAMPLE_DIR/perf -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn" -s 256 -g -q 128 -o 4096 -w write -t 5 -c 0x2
	$SPDK_EXAMPLE_DIR/reconnect -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn" -g -q 32 -o 4096 -w randrw -M 50 -t 5 -c 0xE
	$SPDK_EXAMPLE_DIR/arbitration -t 3 -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn" -d 256 -g
	$SPDK_EXAMPLE_DIR/hello_world -d 256 -g -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn"
	$nvmeappdir/overhead/overhead -o 4096 -t 1 -H -g -d 256 -r "trtype:$TEST_TRANSPORT traddr:$test_traddr subnqn:$test_subnqn"
	aer_vfio_user $test_traddr $test_subnqn $i
done

killprocess $nvmfpid

rm -rf /var/run/vfio-user

trap - SIGINT SIGTERM EXIT
