#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

$SPDK_BIN_DIR/spdk_tgt --wait-for-rpc &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# The RPC dsa_scan_accel_module method may be performed to use the DSA Module
# before starting the framework.
# Should FAIL - double-calling of this function is not allowed.
function accel_scan_dsa_modules_test_suite() {
	$rpc_py dsa_scan_accel_module
	NOT $rpc_py dsa_scan_accel_module
}

# The RPC iaa_scan_accel_module method may be performed to use the IAA Module
# before starting the framework.
# Should FAIL - double-calling of this function is not allowed.
function accel_scan_iaa_modules_test_suite() {
	$rpc_py iaa_scan_accel_module
	NOT $rpc_py iaa_scan_accel_module
}

# The RPC accel_assign_opc method may be performed to override the operation code
# assignments to modules before starting the framework.
# Should PASS - opcode assignments can be verified after starting the framework.
function accel_assign_opcode_test_suite() {
	# Assign opc twice, to test replacing the assignment
	$rpc_py accel_assign_opc -o copy -m incorrect

	$rpc_py accel_assign_opc -o copy -m software
	$rpc_py framework_start_init
	$rpc_py accel_get_opc_assignments | jq -r '.copy' | grep software
}

if [[ $CONFIG_IDXD == y && $SPDK_TEST_ACCEL_DSA -gt 0 ]]; then
	run_test "accel_scan_dsa_modules" accel_scan_dsa_modules_test_suite
fi

if [[ $CONFIG_IDXD == y && $SPDK_TEST_ACCEL_IAA -gt 0 ]]; then
	run_test "accel_scan_iaa_modules" accel_scan_iaa_modules_test_suite
fi

run_test "accel_assign_opcode" accel_assign_opcode_test_suite

killprocess $spdk_tgt_pid
