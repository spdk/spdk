#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit
nvmfappstart

subnqn="nqn.2024-07.io.spdk:cnode0"
perf="$SPDK_BIN_DIR/spdk_nvme_perf"

# In-capsule data size smaller than the fabrics connect command (1024) forces usage of control_msg_list. With just one buffer next req must be queued.
$rpc_py nvmf_create_transport "$NVMF_TRANSPORT_OPTS" --in-capsule-data-size 768 --control-msg-num 1
$rpc_py nvmf_create_subsystem "$subnqn" -a
$rpc_py bdev_malloc_create -b Malloc0 32 512
$rpc_py nvmf_subsystem_add_ns "$subnqn" Malloc0
$rpc_py nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# run multiple instanced at once to trigger shortage of the control_msg_list buffers
"$perf" -c 0x2 -q 1 -o 4096 -w randread -t 1 -r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT}" "${NO_HUGE[@]}" &
perf_pid1=$!
"$perf" -c 0x4 -q 1 -o 4096 -w randread -t 1 -r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT}" "${NO_HUGE[@]}" &
perf_pid2=$!
"$perf" -c 0x8 -q 1 -o 4096 -w randread -t 1 -r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT}" "${NO_HUGE[@]}" &
perf_pid3=$!

wait $perf_pid1
wait $perf_pid2
wait $perf_pid3

trap - SIGINT SIGTERM EXIT
nvmftestfini
