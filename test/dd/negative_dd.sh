#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

invalid_arguments() {
	# Invalid arguments
	NOT "${DD_APP[@]}" \
		--ii="" \
		--ob=""
}

double_input() {
	# You may specify either --if or --ib, but not both.
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--ib="$bdev0" \
		--ob="$bdev1"
}

double_output() {
	# You may specify either --of or --ob, but not both
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--ob="$bdev0"
}

no_input() {
	# You must specify either --if or --ib
	NOT "${DD_APP[@]}" \
		--ob="$bdev0"
}

no_output() {
	# You must specify either --of or --ob
	NOT "${DD_APP[@]}" \
		--if="$test_file0"
}

wrong_blocksize() {
	# You must specify bs != 0
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--bs=0
}

smaller_blocksize() {
	# Cannot allocate memory, try smaller block size value
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--bs=99999999999999
}

invalid_count() {
	# Invalid --count value
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--count="-9"
}

invalid_oflag() {
	# --oflag may be used only with --of
	NOT "${DD_APP[@]}" \
		--ib="$bdev0" \
		--ob="$bdev0" \
		--oflag=0
}

invalid_iflag() {
	# --iflag may be used only with --if
	NOT "${DD_APP[@]}" \
		--ib="$bdev0" \
		--ob="$bdev0" \
		--iflag=0
}

unknown_flag() {
	# Unknown file flag
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--oflag="-1"
}

invalid_json() {
	# Both files need to exist and be accessible
	NOT "${DD_APP[@]}" \
		--if="$test_file0" \
		--of="$test_file1" \
		--json <(:)
}

test_file0=$SPDK_TEST_STORAGE/dd.dump0
test_file1=$SPDK_TEST_STORAGE/dd.dump1

touch "$test_file0"
touch "$test_file1"

run_test "dd_invalid_arguments" invalid_arguments
run_test "dd_double_input" double_input
run_test "dd_double_output" double_output
run_test "dd_no_input" no_input
run_test "dd_no_output" no_output
run_test "dd_wrong_blocksize" wrong_blocksize
run_test "dd_smaller_blocksize" smaller_blocksize
run_test "dd_invalid_count" invalid_count
run_test "dd_invalid_oflag" invalid_oflag
run_test "dd_invalid_iflag" invalid_iflag
run_test "dd_unknown_flag" unknown_flag
run_test "dd_invalid_json" invalid_json
