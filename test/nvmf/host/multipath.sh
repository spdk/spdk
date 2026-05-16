#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname $0)")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
TPOINT_NAME=TCP_REQ_EXECUTED
NVMF_APP_TRACE_ARG="-e nvmf_tcp:$TPOINT_NAME"
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

# Filter out admin qpair (qid:0) since TCP_REQ_EXECUTED fires for all
# commands including admin (e.g. AEN, Get Log Page), but we only care
# about IO traffic.
function _spdk_trace_io() {
	"$SPDK_BIN_DIR/spdk_trace" -s nvmf -i $NVMF_APP_SHM_ID \
		| grep "$TPOINT_NAME" | grep -v "qid:0"
}

function _confirm_io_on_port() {
	local port

	# "inaccessible inaccessible" case -- no IO expected on any port.
	if (($# == 0)); then
		_spdk_trace_io | grep -q . && return 1
		return 0
	fi

	for port; do
		_spdk_trace_io | grep -q " ${NVMF_FIRST_TARGET_IP}:${port} " || return 1
	done
}

# check for io on the expected ANA state port
function confirm_io_on_port() {
	local state=$1 actual_port=$2

	$rpc_py trace_clear

	active_port=$($rpc_py nvmf_subsystem_get_listeners $NQN | jq -r '.[] | select (.ana_states[0].ana_state=="'$state'") | .address.trsvcid')

	waitforcondition "_confirm_io_on_port $active_port $actual_port"
}

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -t 120 -s $bdevperf_rpc_sock perform_tests &

sleep 1

# Set ANA state to each listener
set_ANA_state non_optimized optimized
# Check the IO on expected port with ANA state set
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Check traffic paths with different ANA states
set_ANA_state non_optimized inaccessible
confirm_io_on_port "non_optimized" $NVMF_PORT

set_ANA_state inaccessible optimized
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Not expecting the io on any port
set_ANA_state inaccessible inaccessible
confirm_io_on_port "" ""

set_ANA_state non_optimized optimized
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

# Remove listener to monitor multipath function
$rpc_py nvmf_subsystem_remove_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
sleep 1

# Expect IO on alternate path
confirm_io_on_port "non_optimized" $NVMF_PORT

# Add listener back with optimized state, traffic should switch to optimized port
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
$rpc_py nvmf_subsystem_listener_set_ana_state $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT -n optimized

# wait for the io to switch to second port
sleep 6
confirm_io_on_port "optimized" $NVMF_SECOND_PORT

killprocess $bdevperf_pid
# Make sure we catch bdevperf's exit status
wait $bdevperf_pid

cat "$testdir/try.txt"

$rpc_py nvmf_delete_subsystem $NQN

trap - SIGINT SIGTERM EXIT

rm -f "$testdir/try.txt"
nvmftestfini
