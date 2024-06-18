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

function fuzzer_out_handler() {
	if [[ -n $SEND_LLVM_FUZZER_TO_SYSLOG ]]; then
		logger -p user.debug -t LLVM
	elif [[ -n $COMPRESS_LLVM_FUZZER ]]; then
		gzip -c > "$1.gz"
	else
		cat > "$1"
	fi
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
	local testn_idx idx
	local core
	local pid

	# Calculate time for a single test and multiply it by number
	# of test execute in parallel and round it up to 1 sek per test
	timen=$(printf %d $(((time / fuzz_num) * testn)))
	timen=$((timen == 0 ? 1 : timen))

	for ((i = 0; i < fuzz_num; i += testn)); do
		idx=-1 pids=()
		# Run max up to $testn tests in parallel ...
		while ((testn_idx = i + ++idx, testn_idx < fuzz_num && idx < testn)); do
			core=$(printf "0x%x" $((0x1 << idx)))
			start_llvm_fuzz $testn_idx $timen $core &> >(fuzzer_out_handler "$output_dir/llvm/llvm_${FUZZER}_${testn_idx}.txt") &
			pids+=($!)
		done
		# ... and now wait for them
		for pid in "${pids[@]}"; do
			wait "$pid"
		done
		# queue up another $testn bundle at next iteration
	done
}

function start_llvm_fuzz_short() {
	local fuzz_num=$1
	local time=$2

	for ((i = 0; i < fuzz_num; i++)); do
		start_llvm_fuzz $i $time 0x1
	done
}
