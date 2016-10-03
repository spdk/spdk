#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter json

$valgrind $testdir/parse/json_parse_ut
$valgrind $testdir/util/json_util_ut
$valgrind $testdir/write/json_write_ut

timing_exit json
