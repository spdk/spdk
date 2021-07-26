#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "thread_poller_perf" $testdir/poller_perf/poller_perf -b 1000 -l 1 -t 1
run_test "thread_poller_perf" $testdir/poller_perf/poller_perf -b 1000 -l 0 -t 1
