#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit
nvmfappstart -m 0x3

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_null_create NULL1 1000 512
# Subsystem destruction process waits for all controllers to be destroyed which in turn wait
# for all qpairs to be deleted. A qpair can only be deleted when all outstanding requests are completed.
# bdev_delay is used in this test to create outstanding requests when disconnect starts, triggering
# the async qpair/controller/subsystem destruction path.
#
# The delay MUST be longer than perf's -t duration. This guarantees IOs remain in-flight when
# perf's benchmark timer expires, keeping the drain loop actively polling for completions.
# Since subsystem teardown is also blocked on these same delayed IOs, the disconnect cannot
# arrive after polling has stopped — the bdev_delay expiry unblocks both at the same time.
$rpc_py bdev_delay_create -b NULL1 -d Delay0 -r 5000000 -t 5000000 -w 5000000 -n 5000000
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0

run_app_bg "$SPDK_BIN_DIR/spdk_nvme_perf" -c 0xC -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-t 2 -q 128 -w randrw -M 70 -o 512 -P 4
perf_pid=$!

sleep 1

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

waitforcondition '! kill -0 $perf_pid 2>/dev/null'

# Verify perf's exit status to make sure we catch a potential crash
NOT wait "$perf_pid"

#check that traffic goes when a new subsystem is created
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Delay0

run_app_bg "$SPDK_BIN_DIR/spdk_nvme_perf" -c 0xC -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" \
	-t 1 -q 128 -w randrw -M 70 -o 512 -P 4
perf_pid=$!

waitforcondition '! kill -0 $perf_pid 2>/dev/null'

# Verify perf's exit status to make sure we catch a potential crash
wait "$perf_pid"

trap - SIGINT SIGTERM EXIT

nvmftestfini
