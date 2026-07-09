#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Nutanix Inc. All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

TEST_TRANSPORT=tcp
source $rootdir/test/nvmf/common.sh

SUBNQN=nqn.2016-06.io.spdk:cnode1
ECONNREFUSED=111

rpc_py="$rootdir/scripts/rpc.py"
bdevperf_rpc_sock=/var/tmp/bdevperf_sock_error.sock
bdevperf_log="$testdir/bdevperf.log"

function bdevperf_rpc() {
	"$rpc_py" -s "$bdevperf_rpc_sock" "$@"
}

function attach_ctrlr() {
	bdevperf_rpc bdev_nvme_attach_controller \
		-b NVMe0 -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" \
		-f ipv4 -n "$SUBNQN" "$@"
}

function setup_target() {
	local malloc_bdev_size=64 malloc_block_size=512

	"$rpc_py" <<- RPC
		nvmf_create_transport $NVMF_TRANSPORT_OPTS
		bdev_malloc_create $malloc_bdev_size $malloc_block_size -b Malloc0
		nvmf_create_subsystem $SUBNQN -a
		nvmf_subsystem_add_ns $SUBNQN Malloc0
		nvmf_subsystem_add_listener $SUBNQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	RPC
}

function start_initiator() {
	local bdevperf_time=${1:-5}
	shift || :

	run_app_bg "$SPDK_EXAMPLE_DIR/bdevperf" -m 0x4 -z --wait-for-rpc \
		-r "$bdevperf_rpc_sock" -q 32 -o 4096 -w randread -t "$bdevperf_time" \
		&> "$bdevperf_log"
	bdevperf_pid=$!
	waitforlisten "$bdevperf_pid" "$bdevperf_rpc_sock"

	bdevperf_rpc sock_error_register
	bdevperf_rpc sock_set_default_impl -i sock_error
	bdevperf_rpc framework_start_init

	# Apply any extra RPC commands passed as arguments
	local rpc
	for rpc in "$@"; do
		bdevperf_rpc $rpc
	done
}

function stop_initiator() {
	killprocess "$bdevperf_pid"
	bdevperf_pid=
}

# Prints its cntlid if the path is connected, nothing otherwise.
function get_connected_cntlid() {
	bdevperf_rpc bdev_nvme_get_io_paths \
		| jq -r '.poll_groups[0].io_paths[0] | select(.connected) | .cntlid'
}

function io_path_connected() {
	[[ -n "$(get_connected_cntlid)" ]]
}

# A controller reset reconnects with a new cntlid, so a connected io_path with a
# cntlid other than the one seen before proves the reconnect has completed.
function io_path_reconnected() {
	local old_cntlid=$1 cntlid

	cntlid=$(get_connected_cntlid)
	[[ -n "$cntlid" && "$cntlid" != "$old_cntlid" ]]
}

# Ensure correct handling of connect failure in nvme_tcp
function test_connect_failure() {
	local errcode=$ECONNREFUSED out

	start_initiator 1

	bdevperf_rpc sock_error_inject_error \
		--operation connect --type failure --count 1 --errcode "$errcode"

	# Try attaching with expected connect failure. Properly handled
	# it should result in immediate fail with code -5. If this timeouts
	# most likely qpair state was not updated on connect failure.
	if out=$(bdevperf_rpc -t 5 bdev_nvme_attach_controller \
		-b NVMe0 -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" \
		-f ipv4 -n "$SUBNQN" 2>&1); then
		return 1
	fi
	[[ $out == *"Got JSON-RPC error response"* && $out == *'"code": -5'* ]]

	# Next attach should succeed.
	attach_ctrlr

	# Prove the data path still works once injection is exhausted
	"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bdevperf_rpc_sock" perform_tests

	bdevperf_rpc bdev_nvme_detach_controller NVMe0

	stop_initiator
}

# Ensure correct handling of malformed PDU in nvme_tcp
function test_term_req_disconnect() {
	start_initiator 15 "bdev_nvme_set_options --bdev-retry-count 100"

	attach_ctrlr \
		--ctrlr-loss-timeout-sec -1 --reconnect-delay-sec 1 --fast-io-fail-timeout-sec 2
	[[ "$(bdevperf_rpc bdev_nvme_get_controllers | jq -r '.[].name')" == "NVMe0" ]]
	[[ "$(bdevperf_rpc bdev_get_bdevs | jq -r '.[].name')" == "NVMe0n1" ]]

	timeout 60 "$rootdir/examples/bdev/bdevperf/bdevperf.py" \
		-s "$bdevperf_rpc_sock" perform_tests &
	local perf_pid=$!

	# The qpair must be RUNNING before we inject corruption,
	# otherwise it hits the IC_RESP setup path instead of the H2C term request path.
	waitforcondition io_path_connected 10
	local cntlid
	cntlid=$(get_connected_cntlid)
	[[ -n "$cntlid" ]]

	# Flip the pdu_type of common header to yield an invalid PDU type error
	# and force the host to send an H2C term request.
	local common_pdu_header_size=8
	bdevperf_rpc sock_error_inject_error \
		--operation recv --type corrupt --count 1 --match-len "$common_pdu_header_size"

	# Without the fix the qpair is never disconnected, so no reset and no new cntlid.
	waitforcondition "io_path_reconnected $cntlid" 10

	# The corruption forced an H2C term request; the fix under test must disconnect the
	# qpair and let bdevperf recover (via retries) rather than hang with outstanding I/O.
	wait "$perf_pid"

	bdevperf_rpc bdev_nvme_detach_controller NVMe0

	stop_initiator
}

function cleanup() {
	if [[ -n "${bdevperf_pid:-}" ]]; then
		cat "$bdevperf_log"
		kill -9 "$bdevperf_pid" 2> /dev/null || :
		wait "$bdevperf_pid" 2> /dev/null || :
	fi
	process_shm --id "$NVMF_APP_SHM_ID" || :
	"$rpc_py" nvmf_delete_subsystem "$SUBNQN" 2> /dev/null || :
	nvmftestfini
}

nvmftestinit
nvmfappstart -m 0x3
trap 'cleanup; exit 1' SIGINT SIGTERM EXIT

setup_target

run_test "test_connect_failure" test_connect_failure
run_test "test_term_req_disconnect" test_term_req_disconnect

trap - SIGINT SIGTERM EXIT
nvmftestfini
