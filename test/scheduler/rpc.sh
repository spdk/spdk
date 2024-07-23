#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

rpc=rpc_cmd

function framework_get_governor() {
	"${SPDK_APP[@]}" -m "$spdk_cpumask" &
	spdk_pid=$!
	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid

	# Static scheduler does not use any governor
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "static" ]]
	[[ -z "$($rpc framework_get_governor | jq -r '.[]')" ]]

	# gscheduler uses the only currently implemented governor - dpdk_governor
	$rpc framework_set_scheduler gscheduler
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "gscheduler" ]]
	[[ "$($rpc framework_get_governor | jq -r '.governor_name')" == "dpdk_governor" ]]

	# Check that core length matches the cpumask and first one is the main core
	[[ "$($rpc framework_get_governor | jq -r '.cores | length')" -eq "$spdk_cpus_no" ]]
	[[ "$($rpc framework_get_governor | jq -r '.cores[0].lcore_id')" -eq "$spdk_main_core" ]]
	[[ -n "$($rpc framework_get_governor | jq -r '.cores[0].current_frequency')" ]]

	# dpdk_governor always has an env it uses
	[[ -n "$($rpc framework_get_governor | jq -r '.module_specific.env')" ]]

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

function scheduler_opts() {
	"${SPDK_APP[@]}" -m "$spdk_cpumask" --wait-for-rpc &
	spdk_pid=$!
	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid

	# It should not be possible to change settings that a scheduler does not support
	NOT $rpc framework_set_scheduler static --core-limit 42

	# It is possible to change settings generic scheduler opts for schedulers in event framework
	$rpc framework_set_scheduler dynamic -p 424242
	[[ "$($rpc framework_get_scheduler | jq -r '. | select(.scheduler_name == "dynamic") | .scheduler_period')" -eq 424242 ]]

	# Verify that the scheduler is changed and the non-default value is set
	$rpc framework_set_scheduler dynamic --core-limit 42
	[[ "$($rpc framework_get_scheduler | jq -r '. | select(.scheduler_name == "dynamic") | .core_limit')" -eq 42 ]]

	# Switch scheduler back and forth and verify values are kept (scheduler implementation specific)
	$rpc framework_set_scheduler gscheduler
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "gscheduler" ]]
	$rpc framework_set_scheduler dynamic
	[[ "$($rpc framework_get_scheduler | jq -r '. | select(.scheduler_name == "dynamic") | .core_limit')" -eq 42 ]]

	# All the above configuration can happen before subsystems initialize
	$rpc framework_start_init

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

function static_as_default() {
	"${SPDK_APP[@]}" -m "$spdk_cpumask" --wait-for-rpc &
	spdk_pid=$!
	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid

	# Before initialization scheduler is set to NULL. If unchanged, set to static
	# during subsystem initialization.
	[[ "$($rpc framework_get_scheduler | jq -r '. | select(.scheduler_name == null)')" ]]
	$rpc framework_start_init
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "static" ]]

	# It should never be possible to return to static scheduler after changing it
	$rpc framework_set_scheduler dynamic
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "dynamic" ]]
	NOT $rpc framework_set_scheduler static
	[[ "$($rpc framework_get_scheduler | jq -r '.scheduler_name')" == "dynamic" ]]

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

run_test "scheduler_opts" scheduler_opts
run_test "static_as_default" static_as_default
run_test "framework_get_governor" framework_get_governor
