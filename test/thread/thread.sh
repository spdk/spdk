#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "thread_poller_perf" $testdir/poller_perf/poller_perf -b 1000 -l 1 -t 1
run_test "thread_poller_perf" $testdir/poller_perf/poller_perf -b 1000 -l 0 -t 1

# spdk_lock.c includes thread.c, which causes problems when registering the same
# tracepoint for "thread" in the program and shared library. It is sufficient
# to test this only on static builds.
if [[ "$CONFIG_SHARED" != "y" ]]; then
	run_test "thread_spdk_lock" $testdir/lock/spdk_lock
fi
