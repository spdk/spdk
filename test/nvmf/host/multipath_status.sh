#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  Copyright (C) 2024 Samsung Electronics Co., Ltd.
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname $0)")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
bdevperf_rpc_sock=/var/tmp/bdevperf.sock

# NQN prefix to use for subsystem NQNs
NQN=nqn.2016-06.io.spdk:cnode1

cleanup() {
	process_shm --id $NVMF_APP_SHM_ID || true
	cat "$testdir/try.txt"
	rm -f "$testdir/try.txt"
	killprocess $bdevperf_pid
	nvmftestfini
}

nvmftestinit

nvmfappstart -m 0x3
nvmfapp_pid=$!

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
# Set -r to enable ANA reporting feature
$rpc_py nvmf_create_subsystem $NQN -a -s SPDK00000000000001 -r -m 2
$rpc_py nvmf_subsystem_add_ns $NQN Malloc0
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT

run_app_bg "$SPDK_EXAMPLE_DIR/bdevperf" -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 90 &> "$testdir/try.txt"
bdevperf_pid=$!

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

# Create a controller and set multipath behavior
# bdev_retry_count is set to -1 means infinite reconnects
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1
# -l -1 ctrlr_loss_timeout_sec -1 means infinite reconnects
# -o 10 reconnect_delay_sec time to delay a reconnect retry is limited to 10 sec
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n $NQN -x multipath -l -1 -o 10
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b Nvme0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -f ipv4 -n $NQN -x multipath -l -1 -o 10

function set_ANA_state() {
	$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n $1
	$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -n $2
}

function check_status() {
	local io_paths
	io_paths=$($rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_io_paths)

	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_PORT\").current" <<< "$io_paths") == "$1" ]]
	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_SECOND_PORT\").current" <<< "$io_paths") == "$2" ]]
	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_PORT\").connected" <<< "$io_paths") == "$3" ]]
	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_SECOND_PORT\").connected" <<< "$io_paths") == "$4" ]]
	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_PORT\").accessible" <<< "$io_paths") == "$5" ]]
	[[ $(jq -r ".poll_groups[].io_paths[] | select(.transport.trsvcid==\"$NVMF_SECOND_PORT\").accessible" <<< "$io_paths") == "$6" ]]
}

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -t 120 -s $bdevperf_rpc_sock perform_tests &

sleep 2

# check_status takes 6 parameters for expected values:
# 1: "current" for port 1
# 2: "current" for port 2
# 3: "connected" for port 1
# 4: "connected" for port 2
# 5: "accessible" for port 1
# 6: "accessible" for port 2

# Set ANA state to each listener
# For active/passive, only the first available path should be current
set_ANA_state optimized optimized
sleep 1
check_status true false true true true true

set_ANA_state non_optimized optimized
sleep 1
check_status false true true true true true

# For active/passive, if all paths are non_optimized the first available
# path should be current
set_ANA_state non_optimized non_optimized
sleep 1
check_status true false true true true true

set_ANA_state non_optimized inaccessible
sleep 1
check_status true false true true true false

set_ANA_state inaccessible inaccessible
sleep 1
check_status false false true true false false

set_ANA_state inaccessible optimized
sleep 1
check_status false true true true false true

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_multipath_policy -b Nvme0n1 -p active_active

# For active/active, all optimized paths should be current
set_ANA_state optimized optimized
sleep 1
check_status true true true true true true

set_ANA_state non_optimized optimized
sleep 1
check_status false true true true true true

# For active/active, all non-optimized paths should be current if there are
# no optimized paths
set_ANA_state non_optimized non_optimized
sleep 1
check_status true true true true true true

set_ANA_state non_optimized inaccessible
sleep 1
check_status true false true true true false

killprocess $bdevperf_pid
# Make sure we catch bdevperf's exit status
wait $bdevperf_pid

cat "$testdir/try.txt"

$rpc_py nvmf_delete_subsystem $NQN

trap - SIGINT SIGTERM EXIT

rm -f "$testdir/try.txt"
nvmftestfini
