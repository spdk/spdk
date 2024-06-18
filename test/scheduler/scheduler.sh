#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/isolate_cores.sh"

"$rootdir/scripts/setup.sh"

run_test "idle" "$testdir/idle.sh"
#run_test "load_balancing" "$testdir/load_balancing.sh"
run_test "dpdk_governor" "$testdir/governor.sh"
run_test "interrupt_mode" "$testdir/interrupt.sh"
