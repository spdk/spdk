#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"

fuzzers=($(get_fuzzer_targets))

llvm_out=$output_dir/llvm

mkdir -p $rootdir/../corpus/ $llvm_out

function lcov_start() {
	local out=$llvm_out
	local src=$rootdir

	if hash lcov; then
		export LCOV_OPTS="
			--rc lcov_branch_coverage=1
			--rc lcov_function_coverage=1
			--rc genhtml_branch_coverage=1
			--rc genhtml_function_coverage=1
			--rc genhtml_legend=1
			--rc geninfo_all_blocks=1
			--gcov-tool $rootdir/test/fuzz/llvm/llvm-gcov.sh
			"
		export LCOV="lcov $LCOV_OPTS --no-external"

		# Print lcov version to log
		$LCOV -v
		# zero out coverage data
		$LCOV -q -c -i -t "Baseline" -d $src -o $out/cov_base.info
	fi
}

function lcov_stop() {
	local out=$llvm_out
	local src=$rootdir

	if hash lcov; then
		# generate coverage data and combine with baseline
		$LCOV -q -c -d $src -t "$(hostname)" -o $out/cov_test.info
		$LCOV -q -a $out/cov_base.info -a $out/cov_test.info -o $out/cov_total.info
		$LCOV -q -r $out/cov_total.info '*/dpdk/*' -o $out/cov_total.info
		$LCOV -q -r $out/cov_total.info '/usr/*' -o $out/cov_total.info
		owner=$(stat -c "%U" .)
		sudo -u $owner git clean -f "*.gcda"
		rm -f cov_base.info cov_test.info OLD_STDOUT OLD_STDERR
	fi
}

# Collect coverage data when run fuzzers for longer period of time
# this allow to check coverage progression between runs, and grow of corpus files
if [[ $SPDK_TEST_FUZZER_SHORT -eq 0 ]]; then
	lcov_start
fi

for fuzzer in "${fuzzers[@]}"; do
	case "$fuzzer" in
		nvmf) run_test "nvmf_fuzz" "$testdir/llvm/$fuzzer/run.sh" ;;
		vfio) run_test "vfio_fuzz" "$testdir/llvm/$fuzzer/run.sh" ;;
	esac
done

if [[ $SPDK_TEST_FUZZER_SHORT -eq 0 ]]; then
	lcov_stop
	genhtml $llvm_out/cov_total.info --output-directory $llvm_out
fi
