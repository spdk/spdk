#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

bdevperf_rpc_sock=/var/tmp/bdevperf.sock

nvmftestinit

nvmfappstart -m 0xE

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT

$rootdir/build/examples/bdevperf -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 10 -f &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; cat $testdir/try.txt; rm -f $testdir/try.txt; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
run_test_pid=$!

sleep 1

$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

sleep 3

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT

sleep 3

# Give the admin qpair time to fail before we add the new listener in. This prevents us from trying to connect to the wrong trid.
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

sleep 1

$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT

wait $run_test_pid

killprocess $bdevperf_pid

cat $testdir/try.txt
# if this test fails it means we didn't fail over to the second
count="$(grep -c "Resetting controller successful" < $testdir/try.txt)"

if ((count != 3)); then
	false
fi

# Part 2 of the test. Start removing ports, starting with the one we are connected to, confirm that the ctrlr remains active until the final trid is removed.
$rootdir/build/examples/bdevperf -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 1 -f &> $testdir/try.txt &
bdevperf_pid=$!

waitforlisten $bdevperf_pid $bdevperf_rpc_sock
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -q NVMe0

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_detach_controller NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

# Async operation since we need to reconnect with new TRID.
sleep 3
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -q NVMe0
$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
run_test_pid=$!

wait $run_test_pid

cat $testdir/try.txt
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -q NVMe0

# No need to wait here since we are deleting a TRID we aren't connected to.
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_detach_controller NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_THIRD_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -q NVMe0
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_detach_controller NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
sleep 3

if $rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -q NVMe0; then
	echo "Controller was not properly removed."
	false
fi

killprocess $bdevperf_pid

sync
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

rm -f $testdir/try.txt
nvmftestfini
