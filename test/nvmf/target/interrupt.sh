#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Dell Inc, or its subsidiaries.
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/interrupt/common.sh

nvmftestinit
nvmfappstart -m 0x3
setup_bdev_aio

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -q 256
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 AIO0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Confirm that with no traffic all cpu cores are idle
for i in {0..1}; do
	reactor_is_idle $nvmfpid $i
done

perf="$SPDK_BIN_DIR/spdk_nvme_perf"

# run traffic
$perf -q 256 -o 4096 -w randrw -M 30 -t 10 -c 0xC \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:nqn.2016-06.io.spdk:cnode1" "${NO_HUGE[@]}" &

perf_pid=$!

# confirm that during load all cpu cores are busy
for i in {0..1}; do
	BUSY_THRESHOLD=30 reactor_is_busy $nvmfpid $i
done

wait $perf_pid

# with no load all cpu cores should be idle again
for i in {0..1}; do
	reactor_is_idle $nvmfpid $i
done

trap - SIGINT SIGTERM EXIT
nvmftestfini
