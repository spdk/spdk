#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"

fuzzers=($(get_fuzzer_targets))

for fuzzer in "${fuzzers[@]}"; do
	case "$fuzzer" in
		nvmf) run_test "nvmf_fuzz" "$testdir/llvm/$fuzzer/run.sh" ;;
		vfio) run_test "vfio_fuzz" "$testdir/llvm/$fuzzer/run.sh" ;;
	esac
done
