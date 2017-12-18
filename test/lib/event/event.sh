#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

timing_enter event
$testdir/event_perf/event_perf -m 0xF -t 1
$testdir/reactor/reactor -t 1
$testdir/reactor_perf/reactor_perf -t 1
report_test_completion "event"
timing_exit event
