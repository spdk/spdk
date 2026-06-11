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
source $rootdir/test/interrupt/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NUM_DEVICES=2

rpc_py="$rootdir/scripts/rpc.py"

export PYTHONPATH=$rootdir/test/nvme/aer
export TEST_TRANSPORT=VFIOUSER

aer_socket="$output_dir/spdk_aer_vfio.sock"
aer_rpc_py="$rpc_py -s $aer_socket --plugin aer_plugin"

function setup_nvmf_vfio_user() {
	local nvmf_app_args=$1
	local transport_args=$2

	"${NVMF_APP[@]}" -m '[2,3]' $nvmf_app_args &
	nvmfpid=$!

	echo "Process pid: $nvmfpid"

	trap 'killprocess $nvmfpid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $nvmfpid

	sleep 1

	$rpc_py nvmf_create_transport -t $TEST_TRANSPORT $transport_args

	mkdir -p /var/run/vfio-user

	for i in $(seq 1 $NUM_DEVICES); do
		mkdir -p /var/run/vfio-user/domain/vfio-user$i/$i

		$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
		$rpc_py nvmf_create_subsystem nqn.2019-07.io.spdk:cnode$i -a -s SPDK$i
		$rpc_py nvmf_subsystem_add_ns nqn.2019-07.io.spdk:cnode$i Malloc$i
		$rpc_py nvmf_subsystem_add_listener nqn.2019-07.io.spdk:cnode$i -t $TEST_TRANSPORT -a "/var/run/vfio-user/domain/vfio-user$i/$i" -s 0
	done
}

function teardown_nvmf_vfio_user() {
	killprocess $nvmfpid

	rm -rf /var/run/vfio-user

	trap - SIGINT SIGTERM EXIT
}

function identify_test() {
	local r=$1
	run_app "$SPDK_BIN_DIR/spdk_nvme_identify" -r "$r" -g -L nvme -L nvme_vfio -L vfio_pci
}

function perf_test() {
	local r=$1
	run_app "$SPDK_BIN_DIR/spdk_nvme_perf" -r "$r" -s 256 -g -q 128 -o 4096 -w rw -M 50 -t 5
}

function examples_test() {
	local r=$1
	run_app "$SPDK_EXAMPLE_DIR/reconnect" -r "$r" -g -q 32 -o 4096 -w randrw -M 50 -t 5 -c 0x3
	run_app "$SPDK_EXAMPLE_DIR/arbitration" -r "$r" -t 3 -d 256 -g
	run_app "$SPDK_EXAMPLE_DIR/hello_world" -r "$r" -d 256 -g
	run_app "$nvmeappdir/overhead/overhead" -r "$r" -o 4096 -t 1 -H -g -d 256
}

function aer_test() {
	local r=$1
	local subnqn=$2
	local malloc_num=Malloc$(($3 + NUM_DEVICES))
	$rpc_py nvmf_get_subsystems

	# Namespace Attribute Notice Tests
	run_app_bg "$nvmeappdir/aer/aer" -r "$r" -n $NUM_DEVICES -g -t $aer_socket
	aerpid=$!
	trap 'killprocess $aerpid; killprocess $nvmfpid; exit 1' SIGINT SIGTERM EXIT

	waitforlisten $aerpid $aer_socket

	# Add a new namespace
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE --name $malloc_num
	$rpc_py nvmf_subsystem_add_ns $subnqn $malloc_num -n $NUM_DEVICES
	$rpc_py nvmf_get_subsystems

	$aer_rpc_py check_changed_namespaces

	wait $aerpid
}

function concurrent_test() {
	local r1=$1 r2=$2
	run_app "$SPDK_BIN_DIR/spdk_nvme_perf" -r "$r1" -r "$r2" -s 256 -g -q 128 -o 4096 -w randrw -M 50 -t 5 -c 0x3
}

function polling_test() {
	local subnqn1=nqn.2019-07.io.spdk:cnode1
	local subnqn2=nqn.2019-07.io.spdk:cnode2
	local r1="trtype:$TEST_TRANSPORT traddr:/var/run/vfio-user/domain/vfio-user1/1 subnqn:$subnqn1"
	local r2="trtype:$TEST_TRANSPORT traddr:/var/run/vfio-user/domain/vfio-user2/2 subnqn:$subnqn2"

	setup_nvmf_vfio_user '' ''

	run_test "nvmf_vfio_user_identify" identify_test "$r1"
	run_test "nvmf_vfio_user_perf" perf_test "$r1"
	run_test "nvmf_vfio_user_examples" examples_test "$r1"
	run_test "nvmf_vfio_user_aer" aer_test "$r1" "$subnqn1" 2
	run_test "nvmf_vfio_user_concurrent" concurrent_test "$r1" "$r2"

	teardown_nvmf_vfio_user
}

function check_reactors_idle() {
	local pid=$1
	local first=$2
	local last=$3
	local i

	for ((i = first; i <= last; i++)); do
		reactor_is_idle $pid $i
	done
}

function interrupt_test() {
	setup_nvmf_vfio_user '--interrupt-mode' '-M -I'

	# FIXME: nvme vfio-user initiator unconditionally mmaps BAR0, which fails with -M
	check_reactors_idle $nvmfpid 2 3

	teardown_nvmf_vfio_user
}

rm -rf /var/run/vfio-user

run_test "nvmf_vfio_user_polling" polling_test
run_test "nvmf_vfio_user_interrupt" interrupt_test
