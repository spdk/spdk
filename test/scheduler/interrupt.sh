#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid"' EXIT

declare -a cpus=()
declare -a cpus_to_collect=()

fold_list_onto_array cpus $(parse_cpu_list <(echo "$spdk_cpus_csv"))
# Normalize the indexes
cpus=("${cpus[@]}")

interrupt() {
	local busy_cpus
	local cpu thread

	local reactor_framework

	cpus_to_collect=("${cpus[@]}")
	collect_cpu_idle

	# Verify that each cpu, except the main cpu, has no threads assigned
	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	for cpu in "${cpus[@]:1}"; do
		[[ -z $(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework") ]]
	done

	# Standard scenario - spdk app is idle, all cpus, except the main cpu, should be
	# switched into interrupt mode. main cpu should remain busy, all remaining cpus
	# should become idle.
	for cpu in "${!is_idle[@]}"; do
		if ((cpu == spdk_main_core)); then
			((is_idle[cpu] == 0)) # main cpu must not be idle
		fi
		if ((cpu != spdk_main_core)); then
			((is_idle[cpu] == 1)) # all cpus except the main cpu must be idle
		fi
	done

	# select 3 cpus except the main one
	busy_cpus=("${cpus[@]:1:3}") threads=()

	# Create busy thread on each of the selected cpus, then verify if given cpu has become busy
	# and that newly created thread was assigned to a proper lcore.
	for cpu in "${busy_cpus[@]}"; do
		threads[cpu]=$(create_thread -n "thread$cpu" -m "$(mask_cpus "$cpu")" -a 100) cpus_to_collect=("$cpu")
		collect_cpu_idle
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		[[ -n $(jq -r "select(.lcore == $cpu) | .lw_threads[] | select(.name == \"thread$cpu\")" <<< "$reactor_framework") ]]
		((is_idle[cpu] == 0))
	done

	# Make all the threads idle and verify if their cpus have become idle as well and if they were
	# moved away out of their lcores.
	for cpu in "${!threads[@]}"; do
		active_thread "${threads[cpu]}" 0
		cpus_to_collect=("$cpu")
		collect_cpu_idle
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		[[ -z $(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework") ]]
		[[ -n $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.name == \"thread$cpu\")" <<< "$reactor_framework") ]]
		((is_idle[cpu] == 1))
	done

	for cpu in "${!threads[@]}"; do
		destroy_thread "${threads[cpu]}"
	done
}

exec_under_dynamic_scheduler "$scheduler" -m "$spdk_cpumask" --main-core "$spdk_main_core"

interrupt
