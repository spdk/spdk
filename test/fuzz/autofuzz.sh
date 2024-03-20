#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source "$rootdir/test/common/autotest_common.sh"

TEST_TIMEOUT=1200

function prepare_config() {
	local allowed_transports
	local test_module=$1

	config_params=("--enable-asan" "--enable-ubsan" "--enable-debug")

	case "$test_module" in
		nvmf)
			allowed_transports=("rdma" "tcp")
			;;
		vhost)
			allowed_transports=("scsi" "blk" "all")
			config_params+=("--with-vhost")
			config_params+=("--with-virtio")
			;;
		iscsi)
			allowed_transports=("tcp")
			config_params+=("--with-iscsi-initiator")
			;;
		*)
			echo "Invalid module specified. Please specify either nvmf, vhost or iscsi." >&2
			return 1
			;;
	esac

	if ! grep -q "$TEST_TRANSPORT" <(printf '%s\n' "${allowed_transports[@]}"); then
		echo "Invalid transport. Please supply one of the following for module: $test_module." >&2
		echo "${allowed_transports[@]}" >&2
		return 1
	fi

	if [[ "$TEST_TRANSPORT" == "rdma" ]]; then
		config_params+=("--with-rdma")
	fi

	printf '%s\n' "${config_params[@]}"
}

function run_fuzzer() {
	local test_module=$1
	# supply --iso to each test module so that it can run setup.sh.
	"$testdir/autofuzz_$test_module.sh" --iso "--transport=$TEST_TRANSPORT" "--timeout=$TEST_TIMEOUT"
}

# These arguments are used in addition to the test arguments in autotest_common.sh
for i in "$@"; do
	case "$i" in
		--module=*)
			TEST_MODULE="${i#*=}"
			;;
		--timeout=*)
			TEST_TIMEOUT="${i#*=}"
			;;
	esac
done

timing_enter autofuzz

config_params=($(prepare_config "$TEST_MODULE"))

timing_enter make
cd "$rootdir"
./configure "${config_params[@]}"
$MAKE $MAKEFLAGS
timing_exit make

timing_enter fuzz_module
run_fuzzer "$TEST_MODULE"
timing_exit fuzz_module

timing_exit autofuzz
