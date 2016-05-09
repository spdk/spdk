#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter json

$testdir/parse/json_parse_ut
$testdir/util/json_util_ut
$testdir/write/json_write_ut

timing_exit json
