#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 Nutanix Inc. All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"
tgt_sock="/var/tmp/tgt.sock"
tgt_rpc="$rpc_py -s $tgt_sock"

add_remove() {
	local nsid=$1 thread=$2
	local current

	for ((i = 1; i <= ns_per_thread; i++)); do
		current=$((nsid + i))
		$tgt_rpc nvmf_subsystem_add_ns -n "$current" "$NVME_SUBNQN" "null${thread}"
		$tgt_rpc nvmf_subsystem_remove_ns "$NVME_SUBNQN" "$current"

		# Check if intiator is still alive, otherwise we'd wait until all threads finish
		kill -s 0 "$spdk_app_pid"
	done
}

nvmftestinit
DEFAULT_RPC_ADDR="$tgt_sock" nvmfappstart -r "$tgt_sock" -m 0x1

$tgt_rpc nvmf_create_transport $NVMF_TRANSPORT_OPTS
$tgt_rpc nvmf_create_subsystem "$NVME_SUBNQN" -a -s SPDK00000000000001 -m 512
$tgt_rpc nvmf_subsystem_add_listener "$NVME_SUBNQN" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

"${SPDK_APP[@]}" -m 0x2 "${NO_HUGE[@]}" &
spdk_app_pid=$!
trap 'killprocess $spdk_app_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten "$spdk_app_pid"

# Run several subsystem_{add,remove}_ns RPCs in parallel to ensure they'll get queued
nthreads=1 pids=()
ns_per_thread=20
bdev_size=100
blk_size=4096

for ((i = 1; i <= nthreads; ++i)); do
	$tgt_rpc bdev_null_create "null${i}" "$bdev_size" "$blk_size"
done

$rpc_py bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -f IPv4 -s "$NVMF_PORT" -n "$NVME_SUBNQN" -b nvme0

for ((i = 1; i <= nthreads; ++i)); do
	# Allocate enough nsids for each thread to not overlap
	add_remove "$((((i - 1) * ns_per_thread) + nthreads + 1))" "$i" &
	pids+=($!)
done
wait "${pids[@]}"

waitforlisten "$spdk_app_pid"
killprocess "$spdk_app_pid"
trap - SIGINT SIGTERM EXIT

nvmftestfini
