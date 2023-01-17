#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

# Store pids of parallel fuzzer tests
pids=()

function cleanup() {
	rm -rf "$@"
	# Make sure that there is no process left hanging
	kill -9 "${pids[@]}" || :
}

function get_testn() {
	local fuzz_num=$1
	local mem_size=$2
	local nproc
	nproc=$(nproc)
	# Choose lower value, best case scenario is one test per core
	local testn=$((fuzz_num < nproc ? fuzz_num : nproc))

	export HUGEMEM=$((mem_size * testn))
	setup
	TESTN=$testn
}

function start_llvm_fuzz_all() {
	local testn=$1    # Number of test to run in parallel
	local fuzz_num=$2 # Number of fuzzer tests
	local time=$3     # Time available for all fuzzers
	local core

	# Calculate time for a single test and multiply it by number
	# of test execute in parallel and round it up to 1 sek per test
	timen=$(printf %d $(((time / fuzz_num) * testn)))
	timen=$((timen == 0 ? 1 : timen))

	for ((i = 0; i < fuzz_num; i++)); do
		core=$(printf "0x%x" $((0x1 << (i % testn))))
		start_llvm_fuzz $i $timen $core &> $output_dir/llvm/llvm_"$FUZZER"_$i.txt &
		pids+=($!)

		# Wait for processes to finish
		if (((i + 1) % testn == 0 || fuzz_num - i - 1 == 0)); then
			(
				sleep $((timen * 10 + 100))
				echo "Timeout $time"
				exit 1
			) &
			timeout_pid=$!
			for pid in "${pids[@]}"; do
				wait $pid
			done
			kill $timeout_pid || :
			pids=()
		fi
	done
}

function start_llvm_fuzz_short() {
	local fuzz_num=$1
	local time=$2

	for ((i = 0; i < fuzz_num; i++)); do
		start_llvm_fuzz $i $time 0x1
	done
}
