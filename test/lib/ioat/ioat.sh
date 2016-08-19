#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

timing_enter ioat

timing_enter unit
$valgrind $testdir/unit/ioat_ut
timing_exit unit

timing_enter perf
$rootdir/examples/ioat/perf/perf -t 1
timing_exit perf

timing_enter verify
$rootdir/examples/ioat/verify/verify -t 1
timing_exit verify

timing_exit ioat
