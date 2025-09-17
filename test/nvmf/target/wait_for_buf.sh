#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Dell Inc, or its subsidiaries.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

FLOW=$1
CLEAN_FLOW=-1

if [ "$FLOW" = "clean_flow" ]; then
	CLEAN_FLOW=1
elif [ "$FLOW" = "dirty_flow" ]; then
	CLEAN_FLOW=0
else
	echo "Usage: $0 clean_flow|dirty_flow"
	exit 1
fi

nvmftestinit
nvmfappstart --wait-for-rpc

subnqn="nqn.2024-07.io.spdk:cnode0"
perf="$SPDK_BIN_DIR/spdk_nvme_perf"
perf_transport_opt="trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT}"
perf_opt=(-q 64 -o 131072 -w randread -r "$perf_transport_opt")

# set the number of available large iobuf entries low enough to trigger buffer allocation retry scenario
$rpc_py accel_set_options --small-cache-size 0 --large-cache-size 0
$rpc_py bdev_set_options --iobuf-small-cache-size 0 --iobuf-large-cache-size 0
$rpc_py iobuf_set_options --large-pool-count 8 --large-bufsize=131072 --small-pool-count 64
$rpc_py framework_start_init
$rpc_py bdev_malloc_create -b Malloc0 32 512
$rpc_py nvmf_create_transport "$NVMF_TRANSPORT_OPTS" -u 8192 --iobuf-large-cache-size 1 --iobuf-small-cache-size 0
$rpc_py nvmf_create_subsystem "$subnqn" -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns "$subnqn" Malloc0
$rpc_py nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# 131072 (io size) equals to the large iobuf size. We have 8 buffers available, so only the very first 8 request can allocate
# all required buffers at once, following requests must wait and go through the iobuf queuing scenario.

if [ "$CLEAN_FLOW" -eq 1 ]; then
	# Clean flow tests if everything works fine with regular traffic scenario.
	run_app "$perf" "${perf_opt[@]}" -t 1
	iobuf_stats=$($rpc_py iobuf_get_stats)
	retry_count=$(echo "$iobuf_stats" | jq -r '.[] | select(.module == "'nvmf_${TEST_TRANSPORT^^}'") | .large_pool.retry')
	if [[ $retry_count -eq 0 ]]; then
		exit 1
	fi
	# small pool should not be used
	retry_count=$(echo "$iobuf_stats" | jq -r '.[] | select(.module == "'nvmf_${TEST_TRANSPORT^^}'") | .small_pool.retry')
	if [[ $retry_count -gt 0 ]]; then
		exit 1
	fi
else
	# Dirty flow tests if target can correctly clean up awaiting requests when initiator is suddenly gone.
	run_app_bg "$perf" "${perf_opt[@]}" -t 10
	perf_pid=$!
	sleep 4
	kill -9 $perfpid || true
fi

trap - SIGINT SIGTERM EXIT
nvmftestfini
