#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#
# Test batch mode with nvmf_delete_subsystem RPCs:
# 1. Create 20 subsystems, delete all 20 with a batch (all should succeed)
# 2. Create 1 subsystem, try to delete it 20 times with a batch (first succeeds, rest fail)

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

num_subsystems=20

verify_subsystem_count() {
	local expected_count=$1
	local subsystem_count

	subsystem_count=$($rpc_py nvmf_get_subsystems \
		| jq 'map(select(.nqn | startswith("nqn.2016-06.io.spdk:cnode"))) | length')
	((expected_count == subsystem_count))
}

test_batch_delete_all() {
	for i in $(seq 1 $num_subsystems); do
		$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$i -a
	done
	verify_subsystem_count "$num_subsystems"

	batch_input=()
	for i in $(seq 1 $num_subsystems); do
		batch_input+=("nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode$i")
	done
	printf '%s\n' "${batch_input[@]}" | "$rootdir/scripts/rpc.py" --batch-mode

	verify_subsystem_count 0
}

test_batch_delete_duplicate() {
	local tmpfile batch_fail_count

	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a

	batch_input=()
	for i in $(seq 1 $num_subsystems); do
		batch_input+=("nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1")
	done
	tmpfile=$(mktemp)
	# First delete succeeds; the remaining num_subsystems-1 fail with "not found".
	{ printf '%s\n' "${batch_input[@]}" | NOT "$rootdir/scripts/rpc.py" --batch-mode; } 2> "$tmpfile"
	# Discard the batch-mode WARNING line; count only JSON error objects.
	batch_fail_count=$(grep -v '^WARNING' "$tmpfile" | jq -s 'length')
	rm -f "$tmpfile"
	((batch_fail_count == num_subsystems - 1))

	verify_subsystem_count 0
}

nvmftestinit
nvmfappstart -m 0x1

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

run_test "batch_delete_all" test_batch_delete_all
run_test "batch_delete_duplicate" test_batch_delete_duplicate

trap - SIGINT SIGTERM EXIT

nvmftestfini
