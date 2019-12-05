#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "event_perf" $testdir/event_perf/event_perf -m 0xF -t 1
run_test "event_reactor" $testdir/reactor/reactor -t 1
run_test "event_reactor_perf" $testdir/reactor_perf/reactor_perf -t 1
