#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid"' EXIT

thread_stats() {
	local thread
	local busy_threads=0

	get_thread_stats

	# Simply verify if threads stay idle
	for thread in "${!thread_map[@]}"; do
		if ((idle[thread] < busy[thread])); then
			printf 'Waiting for %s to become idle\n' "${thread_map[thread]}"
			((++busy_threads))
		elif ((idle[thread] > busy[thread])); then
			printf '%s is idle\n' "${thread_map[thread]}"
		fi
	done

	((busy_threads == 0))
}

idle() {
	local reactor_framework
	local reactors thread
	local cpusmask thread_cpumask
	local threads

	exec_under_dynamic_scheduler "${SPDK_APP[@]}" -m "$spdk_cpusmask" --main-core "$spdk_main_core"

	# The expectation here is that when SPDK app is idle the following is true:
	# - all threads are assigned to main lcore
	# - threads are not being moved between lcores
	# - each thread has a mask pinned to a single cpu

	local all_set

	xtrace_disable
	while ((samples++ < 5)); do
		all_set=0 cpusmask=0
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		threads=($(
			jq -r "select(.lcore == $spdk_main_core) | .lw_threads[].name" <<< "$reactor_framework"
		))

		for thread in "${threads[@]}"; do
			thread_cpumask=0x$(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.name == \"$thread\") | .cpumask" <<< "$reactor_framework")
			((cpusmask |= thread_cpumask))
		done

		printf 'SPDK cpumask: %x Threads cpumask: %x\n' "$spdk_cpusmask" "$cpusmask"
		thread_stats && ((cpusmask == spdk_cpusmask)) && all_set=1
	done

	((all_set == 1))
	xtrace_restore
}

idle
