#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  Copyright (c) 2024 Samsung Electronics Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid"' EXIT

fold_list_onto_array cpus $(parse_cpu_list <(echo "$spdk_cpus_csv"))
# Normalize the indexes
cpus=("${cpus[@]}")
isolated_core=${cpus[1]}
scheduling_core=${cpus[2]}

set_scheduler_options() {
	local isolated_core_mask

	isolated_core_mask=$(mask_cpus ${isolated_core})
	rpc_cmd scheduler_set_options --scheduling-core ${scheduling_core} -i "${isolated_core_mask}"
}

set_scheduler_and_check_thread_status() {
	local isolated_thread_count tmp_count total_thread_count=0 idle_thread_count
	local core_mask reactors

	for cpu in "${cpus[@]}"; do
		core_mask=$(mask_cpus ${cpu})
		create_thread -n "thread${cpu}" -m "${core_mask}" -a 0
	done

	# Get current thread status. All threads are idle.
	reactors=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	isolated_thread_count=$(jq -r "select(.lcore == ${isolated_core}) | .lw_threads | length" <<< "$reactors")
	total_thread_count=$(echo "$reactors" | jq -r "select(.lcore) | .lw_threads | length" | awk '{s+=$1} END {print s}')

	rpc_cmd framework_set_scheduler dynamic

	reactors=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	isolated_thread_ids=($(echo "$reactors" | jq -r "select(.lcore == ${isolated_core}) | .lw_threads" | jq -r '.[].id'))

	# Check if isolated core's thread counts stay the same.
	tmp_count=$(jq -r "select(.lcore == ${isolated_core}) | .lw_threads | length" <<< "$reactors")
	((isolated_thread_count == tmp_count))

	# Check if rest of the idle threads are on the scheduling core.
	idle_thread_count=$(jq -r "select(.lcore == ${scheduling_core}) | .lw_threads| length" <<< "$reactors")
	tmp_count=$((total_thread_count - isolated_thread_count))

	# Make the thread on the isolated core busy and verify that it remains isolated.
	for thread_id in "${isolated_thread_ids[@]}"; do
		active_thread "$thread_id" 95
	done

	reactors=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	idle_thread_ids=($(echo "$reactors" | jq -r "select(.lcore == ${scheduling_core}) | .lw_threads" | jq -r '.[].id'))
	# Make the threads on the scheudling core busy and verify that they are distributed.
	for thread_id in "${idle_thread_ids[@]}"; do
		if ((thread_id == 1)); then
			continue
		fi
		active_thread "$thread_id" 80
	done
	sleep 20

	reactors=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	tmp_count=$(jq -r "select(.lcore == ${isolated_core}) | .lw_threads | length" <<< "$reactors")
	((isolated_thread_count == tmp_count))

	tmp_count=$(jq -r "select(.lcore == ${scheduling_core}) | .lw_threads| length" <<< "$reactors")
	((idle_thread_count >= tmp_count))
}

exec_under_static_scheduler "$scheduler" -m "$spdk_cpumask" --main-core "$spdk_main_core"

set_scheduler_options

rpc_cmd framework_start_init

set_scheduler_and_check_thread_status
