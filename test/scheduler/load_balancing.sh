#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid"' EXIT

fold_list_onto_array cpus $(parse_cpu_list <(echo "$spdk_cpus_csv"))
# Normalize the indexes
cpus=("${cpus[@]}")

busy() {
	local selected_cpus cpu
	local reactor_framework
	local threads thread

	# Create two busy threads with two cpus (not including main cpu) and check if either of
	# them is moved to either of the selected cpus. Expected load is ~100% on each thread and
	# each thread should remain on its designated cpu.

	fold_list_onto_array selected_cpus "${cpus[@]:1:2}"

	thread0=$(create_thread -n "thread0" -m "$(mask_cpus "${selected_cpus[@]}")" -a 100)
	thread1=$(create_thread -n "thread1" -m "$(mask_cpus "${selected_cpus[@]}")" -a 100)

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

	local thread cpu
	local extra_threads
	local sched_period=1 # default, 1s
	local active_cpu

	# Exclude main cpu
	fold_list_onto_array selected_cpus "${cpus[@]:1}"

	thread0=$(create_thread -n "thread0" -m "$(mask_cpus "${selected_cpus[@]}")" -a 0)
	for cpu in "${selected_cpus[@]::${#selected_cpus[@]}-1}"; do
		extra_threads+=("$(create_thread -n "thread_cpu_$cpu" -m "$(mask_cpus "$cpu")" -a 100)")
	done

	# thread0 is idle, wait for scheduler to run (2x scheduling period) and check if it is on main core
	sleep $((2 * sched_period))
	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	[[ -n $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.id == $thread0)") ]] <<< "$reactor_framework"

	# thread0 is active, wait for scheduler to run (2x) and check if it is not on main core
	active_thread "$thread0" 100
	sleep $((2 * sched_period))
	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')

	[[ -z $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.id == $thread0)") ]] <<< "$reactor_framework"
	# Get the cpu thread was scheduled onto
	for cpu in "${selected_cpus[@]}"; do
		[[ -n $(jq -r "select(.lcore == $cpu) | .lw_threads[] | select(.id == $thread0)") ]] <<< "$reactor_framework" && active_cpu=$cpu
	done
	[[ -n ${selected_cpus[active_cpu]} ]]

	# thread0 is idle, wait for scheduler to run (2x) and check if it is on main core
	active_thread "$thread0" 0
	sleep $((2 * sched_period))
	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')

	[[ -n $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.id == $thread0)") ]] <<< "$reactor_framework"

	# thread0 is active, wait for scheduler to run (2x) and check if it is not on main core
	active_thread "$thread0" 100
	sleep $((2 * sched_period))
	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')

	[[ -z $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.id == $thread0)") ]] <<< "$reactor_framework"

	destroy_thread "$thread0"
	for thread in "${extra_threads[@]}"; do
		destroy_thread "$thread"
	done
}

core_load() {
	local sched_period=1 # default, 1s
	local thread
	local on_main_core=0 on_next_core=0

	# Re-exec the scheduler app to make sure rr balancer won't affect threads without
	# configured cpumask from the previous test suites.

	exec_under_dynamic_scheduler "$scheduler" -m "$spdk_cpumask" --main-core "$spdk_main_core"

	# Create thread0 with 90% activity no cpumask, expecting it to remain on main cpu
	thread0=$(create_thread -n "thread0" -a 90)

	sleep $((2 * sched_period))
	update_thread_cpus_map

	((thread_cpus[thread0] == spdk_main_core))

	# Create thread1 with 90% activity. Expecting one of the threads to be moved to next
	# cpu and the other remain on main cpu. Verifying that threads are spread out when core
	# load is over 95% limit.
	thread1=$(create_thread -n "thread1" -a 90)

	# Three iterations are needed, as both active threads first are moved out of main core.
	# During next scheduling period one of them is moved back to the main core.
	sleep $((3 * sched_period))
	update_thread_cpus_map

	((thread_cpus[thread0] == spdk_main_core || thread_cpus[thread1] == spdk_main_core))
	((thread_cpus[thread0] != thread_cpus[thread1]))

	# Create thread2 with 10% activity. Expecting the idle thread2 to be placed on main cpu and two
	# other active threads on next cpus. Verifying the condition where core load over 95% moves threads
	# away from main cpu.
	thread2=$(create_thread -n "thread2" -a 10)

	sleep $((2 * sched_period))
	update_thread_cpus_map

	((thread_cpus[thread2] == spdk_main_core))
	((thread_cpus[thread1] != spdk_main_core))
	((thread_cpus[thread0] != spdk_main_core))
	((thread_cpus[thread0] != thread_cpus[thread1]))

	# Change all threads activity to 10%. Expecting all threads to be placed on main cpu.
	# Verifying the condition where core load less than 95% is grouping multiple threads.
	active_thread "$thread0" 10
	active_thread "$thread1" 10
	active_thread "$thread2" 10

	sleep $((2 * sched_period))
	update_thread_cpus_map

	for thread in \
		"$thread0" \
		"$thread1" \
		"$thread2"; do
		((thread_cpus[thread] == spdk_main_core))
	done

	# Create thread3, thread4 and thread 5 with 25% activity. Expecting one of the threads on next cpu
	# and rest on main cpu. Total load on main cpu will be (10*3+25*2) 80%, and next cpu 25%.
	thread3=$(create_thread -n "thread3" -a 25)
	thread4=$(create_thread -n "thread4" -a 25)
	thread5=$(create_thread -n "thread5" -a 25)

	# Three iterations are needed, as all threads look active on first iteration since they are on the main core.
	# Second iteration will have them spread out over cores and only third will collapse to the expected scenario.
	sleep $((3 * sched_period))
	update_thread_cpus_map

	# Verify that load is not exceeding 80% on each of the cpus except the main and next cpu
	get_cpu_time 5 user "${cpus[@]:2}"

	for cpu in "${!avg_cpu_time[@]}"; do
		printf '* cpu%u avg load: %u%% (%s)\n' \
			"$cpu" "${avg_cpu_time[cpu]}" "${cpu_times[cpu]}"
		((avg_cpu_time[cpu] <= 80))
	done

	for thread in \
		"$thread0" \
		"$thread1" \
		"$thread2" \
		"$thread3" \
		"$thread4" \
		"$thread5"; do
		if ((thread_cpus[thread] == spdk_main_core)); then
			((++on_main_core))
		else
			((++on_next_core))
		fi

		destroy_thread "$thread"
	done

	((on_main_core == 5 && on_next_core == 1))
}

exec_under_dynamic_scheduler "$scheduler" -m "$spdk_cpumask" --main-core "$spdk_main_core"

run_test "busy" busy
run_test "balanced" balanced
run_test "core_load" core_load
