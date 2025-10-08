#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Nutanix Inc. All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NULL_BDEV_SIZE=64
RESIZED_BDEV_SIZE=128
SUBNQN=nqn.2016-06.io.spdk:cnode1

target_rpc_sock=/var/tmp/spdk.sock
bdevperf_rpc_sock=/var/tmp/bdevperf.sock

# Point rpc_py at the initiator so shared helpers like get_bdev_size work.
rpc_py="$rootdir/scripts/rpc.py -s $bdevperf_rpc_sock"
tgt_rpc="$rootdir/scripts/rpc.py -s $target_rpc_sock"

function get_qpair_count() {
	$tgt_rpc nvmf_subsystem_get_qpairs "$SUBNQN" | jq 'length'
}

function get_bdev_size_s() {
	xtrace_disable
	get_bdev_size "${@}"
	xtrace_restore
}

function cleanup() {
	process_shm --id $NVMF_APP_SHM_ID || :
	[ -f "$bdevperf_log" ] && cat "$bdevperf_log"
	rm -f "$bdevperf_log"
	killprocess $bdevperf_pid || :
	nvmftestfini
}

nvmftestinit
DEFAULT_RPC_ADDR="$target_rpc_sock" nvmfappstart -r "$target_rpc_sock" -m 0x3

# Start bdevperf (initiator), then attach to the already-running target.
bdevperf_log="$testdir/try.txt"
run_app_bg "$SPDK_EXAMPLE_DIR/bdevperf" -m 0x4 -z -r "$bdevperf_rpc_sock" &> "$bdevperf_log"
bdevperf_pid=$!

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT

$tgt_rpc nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$tgt_rpc bdev_null_create null0 "$NULL_BDEV_SIZE" 512
$tgt_rpc bdev_null_create null1 "$NULL_BDEV_SIZE" 512
$tgt_rpc nvmf_create_subsystem "$SUBNQN" --allow-any-host

# Pin NSIDs so remove/add while disconnected maps cleanly to nvme0n1/n2/n3.
$tgt_rpc nvmf_subsystem_add_ns -n 1 "$SUBNQN" null0
$tgt_rpc nvmf_subsystem_add_ns -n 2 "$SUBNQN" null1
$tgt_rpc nvmf_subsystem_add_listener "$SUBNQN" -t "$TEST_TRANSPORT" \
	-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

waitforlisten "$bdevperf_pid" "$bdevperf_rpc_sock"
$rpc_py bdev_nvme_set_options --transport-ack-timeout 1
$rpc_py bdev_nvme_attach_controller -b nvme0 \
	-t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" \
	-f ipv4 -n "$SUBNQN" --ctrlr-loss-timeout-sec -1 --reconnect-delay-sec 1
waitforcondition '$rpc_py bdev_get_bdevs -b nvme0n1 &> /dev/null' 15
waitforcondition '$rpc_py bdev_get_bdevs -b nvme0n2 &> /dev/null' 15
[[ "$(get_bdev_size_s nvme0n1)" == "$NULL_BDEV_SIZE" ]]
[[ "$(get_bdev_size_s nvme0n2)" == "$NULL_BDEV_SIZE" ]]

# Remove the listener to force controller disconnect.
$tgt_rpc nvmf_subsystem_remove_listener "$SUBNQN" -t "$TEST_TRANSPORT" \
	-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforcondition '[[ "$(get_qpair_count)" == 0 ]]' 15

# While listener is disconnected:
# - resize NSID 1 (size refresh on rescan)
# - remove NSID 2 (inactive on target; initiator bdev retained / no depopulate)
# - add NSID 3 (new NS discovered on rescan)
$tgt_rpc bdev_null_resize null0 "$RESIZED_BDEV_SIZE"
$tgt_rpc nvmf_subsystem_remove_ns "$SUBNQN" 2
$tgt_rpc bdev_null_create null2 "$NULL_BDEV_SIZE" 512
$tgt_rpc nvmf_subsystem_add_ns -n 3 "$SUBNQN" null2

# Initiator view must not change until reconnect/rescan.
[[ "$(get_bdev_size_s nvme0n1)" == "$NULL_BDEV_SIZE" ]]
$rpc_py bdev_get_bdevs -b nvme0n2 &> /dev/null
NOT $rpc_py bdev_get_bdevs -b nvme0n3 &> /dev/null

# Restore the listener and verify rescan after reconnect.
$tgt_rpc nvmf_subsystem_add_listener "$SUBNQN" -t "$TEST_TRANSPORT" \
	-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforcondition '[[ "$(get_qpair_count)" != 0 ]]' 15
waitforcondition '[[ "$(get_bdev_size_s nvme0n1)" == "$RESIZED_BDEV_SIZE" ]]' 15
# NSID 2 is removed on the target, but the initiator bdev must remain.
$rpc_py bdev_get_bdevs -b nvme0n2 &> /dev/null
# NSID 3 is added on the target, so the initiator bdev must be created.
waitforcondition '$rpc_py bdev_get_bdevs -b nvme0n3 &> /dev/null' 15
[[ "$(get_bdev_size_s nvme0n3)" == "$NULL_BDEV_SIZE" ]]

# Clean up the target and bdevperf.
killprocess "$bdevperf_pid"
rm -f "$bdevperf_log"
trap - SIGINT SIGTERM EXIT
nvmftestfini
