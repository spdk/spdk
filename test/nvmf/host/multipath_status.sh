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

#
# Multipath configuration tests
#

function check_config() {
	local name=$1
	local expected_attach_count=$2
	local expected_policy=${3:-}
	local expected_selector=${4:-}
	local expected_min_io=${5:-}
	local attach_cfg attach_count mp_opts policy selector min_io

	attach_cfg=$($rpc_py -s $bdevperf_rpc_sock framework_get_config bdev \
		| jq --arg n "$name" \
			'[.[] | select(.method == "bdev_nvme_attach_controller"
			and .params.name == $n)]')

	attach_count=$(jq 'length' <<< "$attach_cfg")
	((attach_count == expected_attach_count))

	mp_opts=$(jq '.[0].params.multipath_opts // {}' <<< "$attach_cfg")

	policy=$(jq -r '.policy // "<missing>"' <<< "$mp_opts")
	selector=$(jq -r '.selector // "<missing>"' <<< "$mp_opts")
	min_io=$(jq -r '.min_io // 0' <<< "$mp_opts")

	if [[ -n "$expected_policy" ]]; then
		[[ "$policy" == "$expected_policy" ]]
	fi
	if [[ -n "$expected_selector" ]]; then
		[[ "$selector" == "$expected_selector" ]]
	fi
	if [[ -n "$expected_min_io" ]]; then
		((min_io == expected_min_io))
	fi
}

function run_bdevperf() {
	run_app_bg "$SPDK_EXAMPLE_DIR/bdevperf" -m 0x4 -z -r $bdevperf_rpc_sock \
		-q 128 -o 4096 -w verify -t 90 &> "$testdir/try.txt"
	bdevperf_pid=$!
	waitforlisten $bdevperf_pid $bdevperf_rpc_sock
}

function stop_bdevperf() {
	killprocess $bdevperf_pid
}

function attach_bdev_nvme_ctrlr() {
	local opts=()

	[[ -n "$2" ]] && opts+=(--policy "$2")
	[[ -n "$3" ]] && opts+=(--selector "$3")
	[[ -n "$4" ]] && opts+=(--min-io "$4")

	$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller \
		-b Nvme0 -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$1" \
		-f ipv4 -n "$NQN" -x multipath "${opts[@]}" &> "$testdir/try.txt"
}

function test_multipath_opts_global() {
	run_bdevperf

	$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options \
		--policy active_active --selector round_robin --min-io 16

	attach_bdev_nvme_ctrlr "$NVMF_PORT"

	check_config Nvme0 1 active_active round_robin 16
	stop_bdevperf
}

function test_multipath_opts_attach() {
	run_bdevperf

	attach_bdev_nvme_ctrlr "$NVMF_PORT" active_active round_robin 4

	check_config Nvme0 1 active_active round_robin 4
	stop_bdevperf
}

function test_multipath_opts_mismatch() {
	run_bdevperf

	attach_bdev_nvme_ctrlr "$NVMF_PORT" active_active round_robin 4
	NOT attach_bdev_nvme_ctrlr "$NVMF_SECOND_PORT" active_passive

	check_config Nvme0 1 active_active round_robin 4
	stop_bdevperf
}

run_test "nvmf_multipath_opts_global" test_multipath_opts_global
run_test "nvmf_multipath_opts_attach" test_multipath_opts_attach
run_test "nvmf_multipath_opts_mismatch" test_multipath_opts_mismatch

cat "$testdir/try.txt"

$rpc_py nvmf_delete_subsystem $NQN

trap - SIGINT SIGTERM EXIT

rm -f "$testdir/try.txt"
nvmftestfini
