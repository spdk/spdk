#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter util

$valgrind $testdir/bit_array/bit_array_ut
$valgrind $testdir/io_channel/io_channel_ut

timing_exit util
