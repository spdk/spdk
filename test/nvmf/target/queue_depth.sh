#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

# Test for if we can allocate a request for the FABRICS_CONNECT when all requests objects are consumed by the queued I/O

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

bdevperf_rpc_sock=/var/tmp/bdevperf.sock

nvmftestinit

nvmfappstart -m 0x2

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rootdir/build/examples/bdevperf -z -r $bdevperf_rpc_sock -q 1024 -o 4096 -w verify -t 10 &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1
$rootdir/examples/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests

# if this test fails it means we didn't fail over to the second

killprocess $bdevperf_pid

trap - SIGINT SIGTERM EXIT

nvmftestfini
