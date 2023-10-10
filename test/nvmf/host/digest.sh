#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2023 Intel Corporation.  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")

set -- "--transport=tcp" "$@"

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

nqn=nqn.2016-06.io.spdk:cnode1
bperfsock=/var/tmp/bperf.sock
runtime=2

bperf_rpc() { "$rootdir/scripts/rpc.py" -s "$bperfsock" "$@"; }
bperf_py() { "$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bperfsock" "$@"; }

cleanup() {
	[[ -n "$bperfpid" ]] && killprocess $bperfpid || :
	nvmftestfini
}

get_transient_errcount() {
	bperf_rpc bdev_get_iostat -b "$1" \
		| jq -r '.bdevs[0]
			| .driver_specific
			| .nvme_error
			| .status_code
			| .command_transient_transport_error'
}

run_bperf() {
	local rw bs qd

	rw=$1 bs=$2 qd=$3
	"$rootdir/build/examples/bdevperf" -m 2 -r "$bperfsock" -w $rw -o $bs -t $runtime -q $qd -z &
	bperfpid=$!

	waitforlisten "$bperfpid" "$bperfsock"
	bperf_rpc bdev_nvme_set_options --nvme-error-stat --bdev-retry-count -1
	# Make sure error injection is disabled when the controller is being attached
	rpc_cmd accel_error_inject_error -o crc32c -t disable
	bperf_rpc bdev_nvme_attach_controller --ddgst -t tcp -a "$NVMF_FIRST_TARGET_IP" \
		-s "$NVMF_PORT" -f ipv4 -n "$nqn" -b nvme0
	# Inject digest errors
	rpc_cmd accel_error_inject_error -o crc32c -t corrupt -i $((qd * 2))

	bperf_py "perform_tests"
	# Make sure that the digest errors were actually caught
	(($(get_transient_errcount nvme0n1) > 0))

	killprocess $bperfpid
}

# This test only makes sense for the TCP transport
[[ "$TEST_TRANSPORT" != "tcp" ]] && exit 1

nvmftestinit
nvmfappstart --wait-for-rpc

rpc_cmd << CONFIG
	accel_assign_opc -o crc32c -m error
	framework_start_init
	bdev_null_create null0 100 4096
	nvmf_create_transport $NVMF_TRANSPORT_OPTS
	nvmf_create_subsystem "$nqn" -a
	nvmf_subsystem_add_ns "$nqn" null0
	nvmf_subsystem_add_listener -t tcp -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" "$nqn"
CONFIG

trap cleanup SIGINT SIGTERM EXIT

# Test the reads - the host should detect digest errors and retry the requests until successful.
run_bperf randread 4096 128
run_bperf randread $((128 * 1024)) 16

trap - SIGINT SIGTERM EXIT
nvmftestfini
