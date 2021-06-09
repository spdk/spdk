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

collect_cpu_stat() {
	local list=$1
	local stat=$2

	for cpu in "${cpus_to_collect[@]}"; do
		eval "${list}[cpu]=\$(get_cpu_stat $cpu $stat)"
	done
}

collect_cpu_idle() {
	xtrace_disable

	local sample_time=${1:-5} samples=0
	local cpu bool inc user_hz

	# idle scales to USER_HZ so we use that in order to determine the expected
	# value it should have been increased to (more or less).
	user_hz=100
	# Expected increase of the idle stat
	inc=$((user_hz * sample_time))

	bool[0]="not" bool[1]="is"

	init_idle_samples=() idle_samples=() is_idle=()

	collect_cpu_stat init_idle_samples idle

	printf 'Collecting cpu idle stats (cpus: %s) for %u seconds...\n' \
		"${cpus_to_collect[*]}" "$sample_time"

	while ((++samples <= sample_time)) && sleep 1s; do
		collect_cpu_stat idle_samples idle
	done

	for cpu in "${!idle_samples[@]}"; do
		# We start to collect after the spdk app is initialized hence if the interrupt
		# mode is not working as expected, the idle time of given cpu will not have a
		# chance to increase. If it does work correctly, then it should change even for
		# a fraction, depending on how much time we spent on collecting this data.
		# If idle time is over 70% of expected increase then we consider this cpu as
		# idle. This is done in order to take into consideration time window the app
		# needs to actually spin up|down the cpu. It's also taken for granted that
		# there is no extra load on the target cpus which may be coming from other
		# processes.
		if ((idle_samples[cpu] > init_idle_samples[cpu] + (inc * 70 / 100))); then
			is_idle[cpu]=1
		else
			is_idle[cpu]=0
		fi
		printf 'cpu%u %s idle (%u %u)\n' \
			"$cpu" "${bool[is_idle[cpu]]}" "${init_idle_samples[cpu]}" "${idle_samples[cpu]}"
	done

	xtrace_restore
}

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
		# Give some extra time for the cpu to spin down
		collect_cpu_idle 10
		reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
		[[ -z $(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework") ]]
		[[ -n $(jq -r "select(.lcore == $spdk_main_core) | .lw_threads[] | select(.name == \"thread$cpu\")" <<< "$reactor_framework") ]]
		((is_idle[cpu] == 1))
	done

	for cpu in "${!threads[@]}"; do
		destroy_thread "${threads[cpu]}"
	done
}

exec_under_dynamic_scheduler "$scheduler" -m "$spdk_cpusmask" --main-core "$spdk_main_core"

interrupt
