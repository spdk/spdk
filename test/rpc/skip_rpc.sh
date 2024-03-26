#!/usr/bin/env bash
#
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

CONFIG_PATH="$rootdir/test/rpc/config.json"
LOG_PATH="$rootdir/test/rpc/log.txt"

test_skip_rpc() {
	$SPDK_BIN_DIR/spdk_tgt --no-rpc-server -m 0x1 &
	local spdk_pid=$!

	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	sleep 5

	NOT rpc_cmd spdk_get_version &> /dev/null
	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

gen_json_config() {
	$SPDK_BIN_DIR/spdk_tgt -m 0x1 &
	local spdk_pid=$!

	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_pid

	# Use an RPC that results in an output to the terminal
	rpc_cmd nvmf_get_transports --trtype tcp || rpc_cmd nvmf_create_transport -t tcp

	rpc_cmd save_config > $CONFIG_PATH
	cat $CONFIG_PATH

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

test_skip_rpc_with_json() {
	gen_json_config

	$SPDK_BIN_DIR/spdk_tgt --no-rpc-server -m 0x1 --json $CONFIG_PATH &> $LOG_PATH &
	local spdk_pid=$!
	sleep 5

	killprocess $spdk_pid
	grep -q "TCP Transport Init" $LOG_PATH
	rm $LOG_PATH
}

test_skip_rpc_with_delay() {
	# Skipping RPC init and adding a delay are exclusive
	NOT $SPDK_BIN_DIR/spdk_tgt --no-rpc-server -m 0x1 --wait-for-rpc
}

test_exit_on_failed_rpc_init() {
	$SPDK_BIN_DIR/spdk_tgt -m 0x1 &
	local spdk_pid=$!
	waitforlisten $spdk_pid

	trap 'killprocess $spdk_pid; exit 1' SIGINT SIGTERM EXIT
	# RPC listening sockets must be unique for each SPDK instance, so this will fail
	NOT $SPDK_BIN_DIR/spdk_tgt -m 0x2

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_pid
}

run_test "skip_rpc" test_skip_rpc
run_test "skip_rpc_with_json" test_skip_rpc_with_json
run_test "skip_rpc_with_delay" test_skip_rpc_with_delay
# Only one DPDK process can run under FreeBSD (see lib/eal/freebsd/eal_hugepage_info.c in DPDK)
if [ $(uname) != "FreeBSD" ]; then
	run_test "exit_on_failed_rpc_init" test_exit_on_failed_rpc_init
fi

rm $CONFIG_PATH
