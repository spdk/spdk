#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid"' EXIT

export PYTHONPATH=$rootdir/test/event/scheduler

declare -r scheduler=$rootdir/test/event/scheduler/scheduler
declare -r plugin=scheduler_plugin

fold_list_onto_array cpus $(parse_cpu_list <(echo "$spdk_cpus_csv"))
# Normalize the indexes
cpus=("${cpus[@]}")

create_thread() {
	"$rootdir/scripts/rpc.py" --plugin "$plugin" scheduler_thread_create "$@"
}

destroy_thread() {
	"$rootdir/scripts/rpc.py" --plugin "$plugin" scheduler_thread_delete "$@"
}

active_thread() {
	"$rootdir/scripts/rpc.py" --plugin "$plugin" scheduler_thread_set_active "$@"
}

busy() {
	local selected_cpus cpu
	local reactor_framework
	local threads thread

	# Create two busy threads with two cpus (not including main cpu) and check if either of
	# them is moved to either of the selected cpus. Expected load is ~100% on each thread and
	# each thread should remain on its designated cpu.

	fold_list_onto_array selected_cpus "${cpus[@]:1:2}"

	thread0_name=thread0
	thread0=$(create_thread -n "$thread0_name" -m "$(mask_cpus "${selected_cpus[@]}")" -a 100)
	thread1_name=thread1
	thread1=$(create_thread -n "$thread1_name" -m "$(mask_cpus "${selected_cpus[@]}")" -a 100)

	local samples=0

	xtrace_disable
	while ((samples++ < 5)); do
		sleep 0.5s

		all_set=0
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')

		printf '*Sample %u\n' "$samples"
		for cpu in "${selected_cpus[@]}"; do
			threads=($(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework"))

			if ((${#threads[@]} == 0)); then
				printf '  No threads found on cpu%u\n' "$cpu"
				continue
			fi

			get_thread_stats

			for thread in "${threads[@]}"; do
				if ((thread != thread0 && thread != thread1)); then
					printf '  Unexpected thread %u (%s) on cpu%u\n' \
						"$thread" "${thread_map[thread]}" "$cpu"
					continue 3
				fi
				load=$((busy[thread] * 100 / (busy[thread] + idle[thread])))
				if ((load < 95)); then
					printf '  Unexpected load on thread %u (%s): %u%% (< 95%%)\n' \
						"$thread" "${thread_map[thread]}" "$load"
					continue 3
				fi
				printf '  Thread %u (%s) on cpu%u; load: %u%%\n' \
					"$thread" "${thread_map[thread]}" "$cpu" "$load"
				eval "${thread_map[thread]}_cpus[$cpu]=$cpu"
			done
		done

		all_set=1
	done

	destroy_thread "$thread0"
	destroy_thread "$thread1"

	# The final expectation is that when target threads are ~100% busy, they will stay on their
	# designated cpus. FIXME: Does it make sense? if given cpu is not getting a break due to a
	# thread not becoming idle even for a tick, scheduler should not put any other threads on
	# that cpu nor move its assigned thread to any other cpu.
	printf 'Thread %u (%s) cpus: %s\n' "$thread0" "${thread_map[thread0]}" "${thread0_cpus[*]:-none}"
	printf 'Thread %u (%s) cpus: %s\n' "$thread1" "${thread_map[thread1]}" "${thread1_cpus[*]:-none}"
	[[ ${thread0_cpus[*]} != "${thread1_cpus[*]}" ]]
	((${#thread0_cpus[@]} == 1 && ${#thread1_cpus[@]} == 1 && all_set == 1))

	xtrace_restore
}

balanced() {
	xtrace_disable

	local thread cpu
	local extra_threads

	# Exclude main cpu
	fold_list_onto_array selected_cpus "${cpus[@]:1}"

	thread0_name=thread0
	thread0=$(create_thread -n "$thread0_name" -m "$(mask_cpus "${selected_cpus[@]}")" -a 0)
	for cpu in "${selected_cpus[@]}"; do
		extra_threads+=("$(create_thread -n "thread_cpu_$cpu" -m "$(mask_cpus "$cpu")" -a 100)")
	done

	while ((samples++ < 5)); do
		# Change active state of the thread0 to make sure scheduler rebalances it across
		# avaialable cpus.
		if ((samples % 2)); then
			active_thread "$thread0" 100
		else
			active_thread "$thread0" 0
		fi
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		printf '* Sample %u\n' "$samples"
		# Include main cpu to check if thread is put back on it
		for cpu in "$spdk_main_core" "${selected_cpus[@]}"; do
			threads=($(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework"))

			if ((${#threads[@]} == 0)); then
				printf '  No threads found on cpu%u\n' "$cpu"
				continue
			fi

			get_thread_stats

			for thread in "${threads[@]}"; do
				load=$((busy[thread] * 100 / (busy[thread] + idle[thread])))
				printf '  Thread %u (%s) on cpu%u; load: %u%%\n' \
					"$thread" "${thread_map[thread]}" "$cpu" "$load"
				eval "${thread_map[thread]}_cpus[$cpu]=$cpu"
			done
		done
	done

	destroy_thread "$thread0"
	for thread in "${extra_threads[@]}"; do
		destroy_thread "$thread"
	done

	# main cpu + at least 2 designated cpus
	((${#thread0_cpus[@]} > 2))
	# main cpu must be present
	[[ -n ${thread0_cpus[spdk_main_core]} ]]
	printf 'Thread %u (%s) rebalanced across cpus: %s\n' \
		"$thread0" "${thread_map[thread0]}" "${thread0_cpus[*]}"

	xtrace_restore
}

exec_under_dynamic_scheduler "$scheduler" -m "$spdk_cpusmask" --main-core "$spdk_main_core"

run_test "busy" busy
run_test "balanced" balanced
