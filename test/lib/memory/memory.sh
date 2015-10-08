#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

timing_enter memory

timing_enter vtophys
$testdir/vtophys
process_core
timing_exit vtophys

timing_exit memory
