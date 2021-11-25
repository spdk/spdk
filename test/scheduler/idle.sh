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
	local cpumask thread_cpumask
	local threads

	exec_under_dynamic_scheduler "${SPDK_APP[@]}" -m "$spdk_cpumask" --main-core "$spdk_main_core"

	# The expectation here is that when SPDK app is idle the following is true:
	# - all threads are assigned to main lcore
	# - threads are not being moved between lcores

	xtrace_disable
	while ((samples++ < 5)); do
		cpumask=0
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		threads=($(
			jq -r "select(.lcore == $spdk_main_core) | .lw_threads[].name" <<< "$reactor_framework"
		))

		for thread in "${threads[@]}"; do
			thread_cpumask=0x$(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.name == \"$thread\") | .cpumask" <<< "$reactor_framework")
			((cpumask |= thread_cpumask))
		done

		printf 'SPDK cpumask: %x Threads cpumask: %x\n' "$spdk_cpumask" "$cpumask"
		thread_stats
	done

	xtrace_restore
}

idle
