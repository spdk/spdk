#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter nvmf

timing_enter unit
$valgrind $testdir/request/request_ut
$valgrind $testdir/session/session_ut
$valgrind $testdir/subsystem/subsystem_ut
timing_exit unit

timing_exit nvmf
