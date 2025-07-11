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

NQN=nqn.2016-06.io.spdk:cnode1

nvmftestinit
nvmfappstart -m 0x3
setup_bdev_aio

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -q 256
$rpc_py nvmf_create_subsystem $NQN -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns $NQN AIO0
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Confirm that with no traffic reactors are idle
for i in {0..1}; do
	reactor_is_idle $nvmfpid $i
done

perf="$SPDK_BIN_DIR/spdk_nvme_perf"

# run traffic
$perf -q 256 -o 4096 -w randrw -M 30 -t 10 -c 0xC \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:${NQN}" "${NO_HUGE[@]}" &

perf_pid=$!

# confirm that during load all reactors are busy
for i in {0..1}; do
	reactor_is_busy $nvmfpid $i
done

wait $perf_pid

# with no traffic all reactors should be idle again
for i in {0..1}; do
	reactor_is_idle $nvmfpid $i
done

# connecting initiator should not cause reactors to be busy
$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n "$NQN" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
for i in {0..1}; do
	reactor_is_idle $nvmfpid $i
done
nvme disconnect -n "$NQN"
waitforserial_disconnect "$NVMF_SERIAL"

trap - SIGINT SIGTERM EXIT
nvmftestfini
