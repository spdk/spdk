#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname $0)")
rootdir=$(readlink -f "$testdir/../../")
source "$rootdir/test/common/autotest_common.sh"

shopt -s nullglob

rpc_sock1=/var/tmp/spdk.sock
rpc_sock2=/var/tmp/spdk2.sock

cleanup() {
	[[ -z $spdk_tgt_pid ]] || killprocess "$spdk_tgt_pid"
	[[ -z $spdk_tgt_pid2 ]] || killprocess "$spdk_tgt_pid2"

	rm -f /var/tmp/spdk_cpu_lock*
}

locks_exist() {
	lslocks -p "$1" | grep -q "spdk_cpu_lock"
}

no_locks() {
	local lock_files=(/var/tmp/spdk_cpu_lock*)
	((${#lock_files[@]} == 0))
}

check_remaining_locks() {
	# Check if locks on cores 0,1,2 are the only remaining ones.
	locks=(/var/tmp/spdk_cpu_lock_*)
	locks_expected=(/var/tmp/spdk_cpu_lock_{000..002})
	[[ ${locks_expected[*]} == "${locks[*]}" ]]
}

default_locks() {
	# Test case 1:
	# Check if files are placed in appropriate directory are locked,
	# and deleted after application closes.
	"${SPDK_APP[@]}" -m 0x1 &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid"

	locks_exist "$spdk_tgt_pid"
	killprocess "$spdk_tgt_pid"

	NOT waitforlisten "$spdk_tgt_pid"

	no_locks
}

default_locks_via_rpc() {
	# Test case 2:
	# Check if files are placed in appropriate directory, locked
	# and deleted after disabling locks via RPCs.
	"${SPDK_APP[@]}" -m 0x1 &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid"

	rpc_cmd framework_disable_cpumask_locks

	no_locks

	rpc_cmd framework_enable_cpumask_locks

	locks_exist "$spdk_tgt_pid"

	killprocess "$spdk_tgt_pid"
}

non_locking_app_on_locked_coremask() {
	# Test case 3:
	# Ensure that SPDK with locks disabled will run on locked mask.
	"${SPDK_APP[@]}" -m 0x1 &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"

	"${SPDK_APP[@]}" -m 0x1 --disable-cpumask-locks -r "$rpc_sock2" &
	spdk_tgt_pid2=$!
	waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	locks_exist "$spdk_tgt_pid"

	killprocess "$spdk_tgt_pid"
	killprocess "$spdk_tgt_pid2"
}

locking_app_on_unlocked_coremask() {
	# Test case 4:
	# Ensure that SPDK application launched with --disable-cpumask-locks
	# allows other app to claim the cores.
	"${SPDK_APP[@]}" -m 0x1 --disable-cpumask-locks &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"

	"${SPDK_APP[@]}" -m 0x1 -r "$rpc_sock2" &
	spdk_tgt_pid2=$!
	waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	locks_exist $spdk_tgt_pid2

	killprocess "$spdk_tgt_pid"
	killprocess "$spdk_tgt_pid2"
}

locking_app_on_locked_coremask() {
	# Test case 5:
	# Ensure that SPDK fails to launch when cores are locked.
	"${SPDK_APP[@]}" -m 0x1 &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"

	"${SPDK_APP[@]}" -m 0x1 -r "$rpc_sock2" &
	spdk_tgt_pid2=$!
	NOT waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	locks_exist $spdk_tgt_pid

	killprocess "$spdk_tgt_pid"
}

locking_overlapped_coremask() {
	# Test case 6:
	# Ensure that even if some cores from the mask are not locked,
	# SPDK refuses to launch due to overlap on the ones with locks.
	"${SPDK_APP[@]}" -m 0x7 &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"

	"${SPDK_APP[@]}" -m 0x1c -r "$rpc_sock2" &
	spdk_tgt_pid2=$!
	NOT waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	check_remaining_locks

	killprocess $spdk_tgt_pid
}

locking_overlapped_coremask_via_rpc() {
	# Test case 7:
	# Ensure that overlapping masks cannot be partially claimed using RPCs.
	"${SPDK_APP[@]}" -m 0x7 --disable-cpumask-locks &
	spdk_tgt_pid=$!
	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"

	"${SPDK_APP[@]}" -m 0x1c -r "$rpc_sock2" --disable-cpumask-locks &
	spdk_tgt_pid2=$!
	waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	rpc_cmd framework_enable_cpumask_locks
	NOT rpc_cmd -s "$rpc_sock2" framework_enable_cpumask_locks

	waitforlisten "$spdk_tgt_pid" "$rpc_sock1"
	waitforlisten "$spdk_tgt_pid2" "$rpc_sock2"

	check_remaining_locks
}

trap 'cleanup' EXIT SIGTERM SIGINT

run_test "default_locks" default_locks
run_test "default_locks_via_rpc" default_locks_via_rpc
run_test "non_locking_app_on_locked_coremask" non_locking_app_on_locked_coremask
run_test "locking_app_on_unlocked_coremask" locking_app_on_unlocked_coremask
run_test "locking_app_on_locked_coremask" locking_app_on_locked_coremask
run_test "locking_overlapped_coremask" locking_overlapped_coremask
run_test "locking_overlapped_coremask_via_rpc" locking_overlapped_coremask_via_rpc

cleanup
